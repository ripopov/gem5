#include <hip/hip_runtime.h>

#include <cstdio>
#include <vector>

using Quad = float __attribute__((ext_vector_type(4)));
constexpr unsigned K = 256;

__global__ void
matmul(const float *a, const float *b, float *c)
{
    const unsigned lane = threadIdx.x;
    Quad acc = {};
    for (unsigned k = 0; k < K; k += 4) {
        const float av = a[(lane % 16) * K + k + lane / 16];
        const float bv = b[(k + lane / 16) * 16 + lane % 16];
        acc = __builtin_amdgcn_mfma_f32_16x16x4f32(av, bv, acc, 0, 0, 0);
        // Preserve an accumulator-resident dependency chain across loads.
        asm volatile("" : "+a"(acc));
    }
    // Exercise accumulator arithmetic independently of AGPR memory operands.
    asm volatile("" : "+v"(acc));
    for (unsigned i = 0; i < 4; ++i) {
        c[(lane / 16 * 4 + i) * 16 + lane % 16] = acc[i];
    }
}

#define HIP_CHECK(call)                                                       \
    do {                                                                      \
        const hipError_t error = (call);                                      \
        if (error != hipSuccess) {                                            \
            std::fprintf(stderr, "%s: %s\n", #call,                           \
                         hipGetErrorString(error));                           \
            return 1;                                                         \
        }                                                                     \
    } while (0)

int
main()
{
    std::vector<float> a(16 * K), b(K * 16), c(16 * 16);
    for (unsigned i = 0; i < a.size(); ++i) {
        a[i] = int(i * 3 % 7) - 3;
        b[i] = int(i * 5 % 11) - 5;
    }
    float *device;
    HIP_CHECK(
        hipMalloc(&device, (a.size() + b.size() + c.size()) * sizeof(float)));
    float *da = device, *db = da + a.size(), *dc = db + b.size();
    HIP_CHECK(hipMemcpy(da, a.data(), a.size() * sizeof(float),
                        hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(db, b.data(), b.size() * sizeof(float),
                        hipMemcpyHostToDevice));
    hipLaunchKernelGGL(matmul, dim3(1), dim3(64), 0, 0, da, db, dc);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());
    HIP_CHECK(hipMemcpy(c.data(), dc, c.size() * sizeof(float),
                        hipMemcpyDeviceToHost));
    HIP_CHECK(hipFree(device));
    for (unsigned row = 0; row < 16; ++row) {
        for (unsigned col = 0; col < 16; ++col) {
            float expected = 0;
            for (unsigned k = 0; k < K; ++k) {
                expected += a[row * K + k] * b[k * 16 + col];
            }
            if (c[row * 16 + col] != expected) {
                std::fprintf(stderr, "FAIL [%u,%u]: got %g, expected %g\n",
                             row, col, c[row * 16 + col], expected);
                return 1;
            }
        }
    }
    std::puts("hip_mfma_accumulator: PASS");
}
