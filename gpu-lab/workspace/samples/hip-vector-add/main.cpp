#include <charconv>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

#include <hip/hip_runtime.h>

#define HIP_CHECK(call)                                                        \
    do {                                                                       \
        hipError_t status = (call);                                             \
        if (status != hipSuccess) {                                             \
            std::cerr << "HIP error: " << hipGetErrorString(status)            \
                      << " at " << __FILE__ << ":" << __LINE__ << "\n";       \
            return 1;                                                          \
        }                                                                      \
    } while (0)

namespace
{

constexpr std::uint32_t blockSize = 64;

bool
parse_size(std::string_view text, std::uint32_t &size)
{
    auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), size);
    return error == std::errc{} && end == text.end() && size > 0;
}

} // namespace

__global__ void
vector_add(const float *a, const float *b, float *c, std::uint32_t n)
{
    std::uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        c[i] = a[i] + b[i];
    }
}

int
main(int argc, char **argv)
{
    std::uint32_t n = 64;
    if (argc != 1 &&
        (argc != 3 || std::string_view(argv[1]) != "--size" ||
         !parse_size(argv[2], n))) {
        std::cerr << "usage: " << argv[0] << " [--size N]\n";
        return 1;
    }

    std::vector<float> a(n), b(n), c(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        a[i] = static_cast<float>(i);
        b[i] = static_cast<float>(2 * i);
    }

    float *da = nullptr;
    float *db = nullptr;
    float *dc = nullptr;
    HIP_CHECK(hipMalloc(&da, n * sizeof(float)));
    HIP_CHECK(hipMalloc(&db, n * sizeof(float)));
    HIP_CHECK(hipMalloc(&dc, n * sizeof(float)));

    HIP_CHECK(hipMemcpy(da, a.data(), n * sizeof(float), hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(db, b.data(), n * sizeof(float), hipMemcpyHostToDevice));

    const std::uint32_t blocks = (n - 1) / blockSize + 1;
    hipLaunchKernelGGL(vector_add, dim3(blocks), dim3(blockSize), 0, 0,
                       da, db, dc, n);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());

    HIP_CHECK(hipMemcpy(c.data(), dc, n * sizeof(float), hipMemcpyDeviceToHost));
    HIP_CHECK(hipFree(da));
    HIP_CHECK(hipFree(db));
    HIP_CHECK(hipFree(dc));

    std::int64_t checksum = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        float expected = a[i] + b[i];
        if (std::fabs(c[i] - expected) > 0.001f) {
            std::cerr << "hip_vector_add: FAIL at " << i << " got " << c[i]
                      << " expected " << expected << "\n";
            return 2;
        }
        checksum += static_cast<std::int64_t>(c[i]);
    }

    std::cout << "hip_vector_add: PASS checksum=" << checksum << "\n";
    return 0;
}
