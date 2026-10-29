#ifndef GPU_LAB_MATMUL_HH
#define GPU_LAB_MATMUL_HH

#include <hip/hip_runtime.h>

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

inline void
hipCheck(hipError_t status)
{
    if (status != hipSuccess) {
        std::cerr << "HIP error: " << hipGetErrorString(status) << '\n';
        std::exit(1);
    }
}

// Shared host contract for the two optimized examples. The default data is
// identical to hip-matmul; --seed exercises less regular integer inputs.
template <class Launch>
int
runMatmul(int argc, char **argv, const char *name, Launch launch,
          unsigned tileM, unsigned tileN, unsigned tileK)
{
    unsigned m = 256, n = 256, k = 128, seed = 0;
    for (int arg = 1; arg < argc; ++arg) {
        std::string_view option(argv[arg]);
        unsigned *value = option == "--m"      ? &m
                          : option == "--n"    ? &n
                          : option == "--k"    ? &k
                          : option == "--seed" ? &seed
                                               : nullptr;
        if (!value || ++arg == argc) {
            std::cerr << "usage: " << argv[0]
                      << " [--m M] [--n N] [--k K] [--seed S]\n";
            return 1;
        }
        std::string_view text(argv[arg]);
        auto [end, error] = std::from_chars(text.begin(), text.end(), *value);
        if (error != std::errc{} || end != text.end() || !*value) {
            std::cerr << "Expected a positive integer\n";
            return 1;
        }
    }
    if (m % tileM || n % tileN || k % tileK || m > 4096 || n > 4096 ||
        k > 4096) {
        std::cerr << "Dimensions must be multiples of " << tileM << ','
                  << tileN << ',' << tileK << " and at most 4096\n";
        return 1;
    }
    std::vector<float> a(size_t(m) * k), b(size_t(k) * n), c(size_t(m) * n);
    uint32_t state = seed;
    auto randomValue = [&]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return float(int(state % 15) - 7);
    };
    for (unsigned row = 0; row < m; ++row) {
        for (unsigned col = 0; col < k; ++col) {
            a[row * k + col] = seed ? randomValue() : float((row + col) % 8);
        }
    }
    for (unsigned row = 0; row < k; ++row) {
        for (unsigned col = 0; col < n; ++col) {
            b[row * n + col] =
                seed ? randomValue() : float((3 * row + col) % 8);
        }
    }
    float *da, *db, *dc;
    hipCheck(hipMalloc(&da, a.size() * sizeof(float)));
    hipCheck(hipMalloc(&db, b.size() * sizeof(float)));
    hipCheck(hipMalloc(&dc, c.size() * sizeof(float)));
    hipCheck(hipMemcpy(da, a.data(), a.size() * sizeof(float),
                       hipMemcpyHostToDevice));
    hipCheck(hipMemcpy(db, b.data(), b.size() * sizeof(float),
                       hipMemcpyHostToDevice));
    launch(da, db, dc, m, n, k);
    hipCheck(hipGetLastError());
    hipCheck(hipDeviceSynchronize());
    hipCheck(hipMemcpy(c.data(), dc, c.size() * sizeof(float),
                       hipMemcpyDeviceToHost));
    hipCheck(hipFree(da));
    hipCheck(hipFree(db));
    hipCheck(hipFree(dc));
    int64_t checksum = 0;
    for (unsigned row = 0; row < m; ++row) {
        for (unsigned col = 0; col < n; ++col) {
            float expected = 0;
            for (unsigned inner = 0; inner < k; ++inner) {
                expected += a[row * k + inner] * b[inner * n + col];
            }
            if (c[row * n + col] != expected) {
                std::cerr << name << ": FAIL at " << row << ',' << col
                          << " got " << c[row * n + col] << " expected "
                          << expected << '\n';
                return 2;
            }
            checksum += int64_t(c[row * n + col]);
        }
    }
    std::cout << name << ": PASS m=" << m << " n=" << n << " k=" << k
              << " seed=" << seed << " checksum=" << checksum << '\n';
    return 0;
}
#endif
