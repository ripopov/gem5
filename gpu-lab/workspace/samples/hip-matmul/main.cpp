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

// A deliberately naive dense matrix multiply: A is m x inner, B is inner x n,
// and C is m x n, all row-major. One work-item computes one output element
// directly from global memory. Each A element is requested n times and each B
// element m times across the grid. Caches can serve repeated requests, but the
// kernel still issues two global loads per multiply-add. You can replace
// those repeated loads with cooperative LDS tiling and register reuse.
__global__ void
matmul(const float *a, const float *b, float *c, std::uint32_t m,
       std::uint32_t n, std::uint32_t inner)
{
    std::uint32_t col = blockIdx.x * blockDim.x + threadIdx.x;
    std::uint32_t row = blockIdx.y * blockDim.y + threadIdx.y;
    if (row >= m || col >= n) {
        return;
    }

    float acc = 0.0f;
    for (std::uint32_t k = 0; k < inner; ++k) {
        acc += a[row * inner + k] * b[k * n + col];
    }
    c[row * n + col] = acc;
}

// The same computation with B held column-major, so the inner loop reads
// bt[col * inner + k] instead of b[k * n + col]. Neighbouring lanes read
// addresses inner floats apart instead of consecutive ones, which turns the
// coalesced access of matmul() into a scattered one. Everything else is
// unchanged, which makes the pair a controlled experiment on access pattern.
__global__ void
matmul_transposed_b(const float *a, const float *bt, float *c, std::uint32_t m,
                    std::uint32_t n, std::uint32_t inner)
{
    std::uint32_t col = blockIdx.x * blockDim.x + threadIdx.x;
    std::uint32_t row = blockIdx.y * blockDim.y + threadIdx.y;
    if (row >= m || col >= n) {
        return;
    }

    float acc = 0.0f;
    for (std::uint32_t k = 0; k < inner; ++k) {
        acc += a[row * inner + k] * bt[col * inner + k];
    }
    c[row * n + col] = acc;
}

int
main(int argc, char **argv)
{
    // Fixed workload shared with both optimized references. Their 64 x 32
    // output tiles give 32 tiles, four per persistent workgroup on eight
    // CUs, and K tiles of 16 give eight inner iterations. This baseline keeps
    // one output per thread in 16 x 16 blocks.
    std::uint32_t m = 256;
    std::uint32_t n = 256;
    std::uint32_t inner = 128;
    bool transpose_b = false;
    std::uint32_t seed = 0;

    for (int arg = 1; arg < argc; ++arg) {
        std::string_view option(argv[arg]);
        if (option == "--transpose-b") {
            transpose_b = true;
        } else if (option == "--size" && arg + 1 < argc &&
                   parse_size(argv[arg + 1], n)) {
            m = inner = n; // Retain square inputs for small debug runs.
            ++arg;
        } else if ((option == "--m" || option == "--n" || option == "--k" ||
                    option == "--seed") &&
                   arg + 1 < argc) {
            auto &value = option == "--m"   ? m
                          : option == "--n" ? n
                          : option == "--k" ? inner
                                            : seed;
            if (!parse_size(argv[++arg], value)) {
                std::cerr << "Expected a positive integer\n";
                return 1;
            }
        } else {
            std::cerr << "usage: " << argv[0]
                      << " [--size N] [--m M] [--n N] [--k K] [--seed S]"
                      << " [--transpose-b]\n";
            return 1;
        }
    }

    if (m > 4096 || n > 4096 || inner > 4096) {
        std::cerr << "Dimensions must be at most 4096\n";
        return 1;
    }

    const std::size_t a_elements = static_cast<std::size_t>(m) * inner;
    const std::size_t b_elements = static_cast<std::size_t>(inner) * n;
    const std::size_t c_elements = static_cast<std::size_t>(m) * n;
    std::vector<float> a(a_elements), b(b_elements), c(c_elements);

    // Values stay in 0..7 so that every product and every partial sum is
    // exactly representable in float. The GPU result can then be compared with
    // the host reference exactly, without a tolerance. B is not symmetric, so
    // storage-order mistakes can be detected by the host reference.
    std::uint32_t state = seed;
    auto random_value = [&]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(static_cast<int>(state % 15) - 7);
    };
    for (std::uint32_t row = 0; row < m; ++row) {
        for (std::uint32_t col = 0; col < inner; ++col) {
            a[row * inner + col] =
                seed ? random_value() : static_cast<float>((row + col) % 8);
        }
    }
    for (std::uint32_t row = 0; row < inner; ++row) {
        for (std::uint32_t col = 0; col < n; ++col) {
            b[row * n + col] = seed ? random_value()
                                    : static_cast<float>((3 * row + col) % 8);
        }
    }

    // The transposed run multiplies the same matrices; only the storage order
    // of B changes, so both runs must produce the same result.
    std::vector<float> b_device_order(b_elements);
    for (std::uint32_t row = 0; row < inner; ++row) {
        for (std::uint32_t col = 0; col < n; ++col) {
            b_device_order[transpose_b ? col * inner + row : row * n + col] =
                b[row * n + col];
        }
    }

    float *da = nullptr;
    float *db = nullptr;
    float *dc = nullptr;
    HIP_CHECK(hipMalloc(&da, a_elements * sizeof(float)));
    HIP_CHECK(hipMalloc(&db, b_elements * sizeof(float)));
    HIP_CHECK(hipMalloc(&dc, c_elements * sizeof(float)));

    HIP_CHECK(hipMemcpy(da, a.data(), a_elements * sizeof(float),
                        hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(db, b_device_order.data(), b_elements * sizeof(float),
                        hipMemcpyHostToDevice));

    const std::uint32_t blocks_x = (n - 1) / blockSide + 1;
    const std::uint32_t blocks_y = (m - 1) / blockSide + 1;
    hipLaunchKernelGGL(transpose_b ? matmul_transposed_b : matmul,
                       dim3(blocks_x, blocks_y), dim3(blockSide, blockSide), 0,
                       0, da, db, dc, m, n, inner);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());

    HIP_CHECK(hipMemcpy(c.data(), dc, c_elements * sizeof(float),
                        hipMemcpyDeviceToHost));
    HIP_CHECK(hipFree(da));
    HIP_CHECK(hipFree(db));
    HIP_CHECK(hipFree(dc));

    std::int64_t checksum = 0;
    for (std::uint32_t row = 0; row < m; ++row) {
        for (std::uint32_t col = 0; col < n; ++col) {
            float expected = 0.0f;
            for (std::uint32_t k = 0; k < inner; ++k) {
                expected += a[row * inner + k] * b[k * n + col];
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

    std::cout << "hip_matmul: PASS m=" << m << " n=" << n << " k=" << inner
              << " layout=" << (transpose_b ? "column-major-b" : "row-major-b")
              << " checksum=" << checksum << "\n";
    return 0;
}
