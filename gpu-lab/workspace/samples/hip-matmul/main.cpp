#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

#include <hip/hip_runtime.h>

#define HIP_CHECK(call)                                                       \
    do {                                                                      \
        hipError_t status = (call);                                           \
        if (status != hipSuccess) {                                           \
            std::cerr << "HIP error: " << hipGetErrorString(status) << " at " \
                      << __FILE__ << ":" << __LINE__ << "\n";                 \
            return 1;                                                         \
        }                                                                     \
    } while (0)

namespace
{

constexpr std::uint32_t blockSide = 16;

bool
parse_size(std::string_view text, std::uint32_t &size)
{
    auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), size);
    return error == std::errc{} && end == text.end() && size > 0;
}

} // namespace

// A deliberately inefficient dense matrix multiply. One work-item computes one
// output element and reads the whole inner product straight from global
// memory, so across the grid every element of A is loaded n times and every
// element of B is loaded n times. The kernel therefore issues two global loads
// per multiply-add, which is the lowest arithmetic intensity a dense matrix
// multiply can have and is far below what a tuned GEMM reaches on the same
// hardware. The inefficiency is the point: it is what makes the memory
// pipeline, the s_waitcnt stalls and the cache hit rates stand out in the
// collected statistics.
__global__ void
matmul(const float *a, const float *b, float *c, std::uint32_t n)
{
    std::uint32_t col = blockIdx.x * blockDim.x + threadIdx.x;
    std::uint32_t row = blockIdx.y * blockDim.y + threadIdx.y;
    if (row >= n || col >= n) {
        return;
    }

    float acc = 0.0f;
    for (std::uint32_t k = 0; k < n; ++k) {
        acc += a[row * n + k] * b[k * n + col];
    }
    c[row * n + col] = acc;
}

// The same computation with B held column-major, so the inner loop reads
// bt[col * n + k] instead of b[k * n + col]. Neighbouring lanes of a wavefront
// then read addresses n floats apart instead of consecutive ones, which turns
// the coalesced access of matmul() into a scattered one. Everything else is
// unchanged, which makes the pair a controlled experiment on access pattern.
__global__ void
matmul_transposed_b(const float *a, const float *bt, float *c, std::uint32_t n)
{
    std::uint32_t col = blockIdx.x * blockDim.x + threadIdx.x;
    std::uint32_t row = blockIdx.y * blockDim.y + threadIdx.y;
    if (row >= n || col >= n) {
        return;
    }

    float acc = 0.0f;
    for (std::uint32_t k = 0; k < n; ++k) {
        acc += a[row * n + k] * bt[col * n + k];
    }
    c[row * n + col] = acc;
}

int
main(int argc, char **argv)
{
    std::uint32_t n = 128;
    bool transpose_b = false;

    for (int arg = 1; arg < argc; ++arg) {
        std::string_view option(argv[arg]);
        if (option == "--transpose-b") {
            transpose_b = true;
        } else if (option == "--size" && arg + 1 < argc &&
                   parse_size(argv[arg + 1], n)) {
            ++arg;
        } else {
            std::cerr << "usage: " << argv[0]
                      << " [--size N] [--transpose-b]\n";
            return 1;
        }
    }

    const std::size_t elements = static_cast<std::size_t>(n) * n;
    std::vector<float> a(elements), b(elements), c(elements);

    // Values stay in 0..7 so that every product and every partial sum is
    // exactly representable in float. The GPU result can then be compared with
    // the host reference exactly, without a tolerance. B is not symmetric, so
    // an index mistake in the kernel cannot go unnoticed.
    for (std::uint32_t row = 0; row < n; ++row) {
        for (std::uint32_t col = 0; col < n; ++col) {
            a[row * n + col] = static_cast<float>((row + col) % 8);
            b[row * n + col] = static_cast<float>((3 * row + col) % 8);
        }
    }

    // The transposed run multiplies the same matrices; only the storage order
    // of B changes, so both runs must produce the same result.
    std::vector<float> b_device_order(elements);
    for (std::uint32_t row = 0; row < n; ++row) {
        for (std::uint32_t col = 0; col < n; ++col) {
            b_device_order[transpose_b ? col * n + row : row * n + col] =
                b[row * n + col];
        }
    }

    float *da = nullptr;
    float *db = nullptr;
    float *dc = nullptr;
    HIP_CHECK(hipMalloc(&da, elements * sizeof(float)));
    HIP_CHECK(hipMalloc(&db, elements * sizeof(float)));
    HIP_CHECK(hipMalloc(&dc, elements * sizeof(float)));

    HIP_CHECK(hipMemcpy(da, a.data(), elements * sizeof(float),
                        hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(db, b_device_order.data(), elements * sizeof(float),
                        hipMemcpyHostToDevice));

    const std::uint32_t blocks = (n - 1) / blockSide + 1;
    hipLaunchKernelGGL(transpose_b ? matmul_transposed_b : matmul,
                       dim3(blocks, blocks), dim3(blockSide, blockSide), 0, 0,
                       da, db, dc, n);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());

    HIP_CHECK(hipMemcpy(c.data(), dc, elements * sizeof(float),
                        hipMemcpyDeviceToHost));
    HIP_CHECK(hipFree(da));
    HIP_CHECK(hipFree(db));
    HIP_CHECK(hipFree(dc));

    std::int64_t checksum = 0;
    for (std::uint32_t row = 0; row < n; ++row) {
        for (std::uint32_t col = 0; col < n; ++col) {
            float expected = 0.0f;
            for (std::uint32_t k = 0; k < n; ++k) {
                expected += a[row * n + k] * b[k * n + col];
            }
            const float got = c[row * n + col];
            if (got != expected) {
                std::cerr << "hip_matmul: FAIL at " << row << "," << col
                          << " got " << got << " expected " << expected
                          << "\n";
                return 2;
            }
            checksum += static_cast<std::int64_t>(got);
        }
    }

    std::cout << "hip_matmul: PASS n=" << n << " layout="
              << (transpose_b ? "column-major-b" : "row-major-b")
              << " checksum=" << checksum << "\n";
    return 0;
}
