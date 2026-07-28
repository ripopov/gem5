#include <cmath>
#include <iostream>
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

__global__ void
scale_input(const float *input, float *intermediate, int n)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        intermediate[i] = 2.0f * input[i];
}

__global__ void
combine(const float *intermediate, const float *b, float *output, int n)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        output[i] = intermediate[i] + b[i];
}

int
main()
{
    constexpr int n = 128;
    constexpr int threads = 64;
    constexpr int blocks = n / threads;
    std::vector<float> input(n), b(n), output(n);
    for (int i = 0; i < n; ++i) {
        input[i] = static_cast<float>(i);
        b[i] = static_cast<float>(3 * i + 1);
    }

    float *device_input = nullptr;
    float *device_b = nullptr;
    float *intermediate = nullptr;
    float *device_output = nullptr;
    hipStream_t stream = nullptr;
    HIP_CHECK(hipMalloc(&device_input, n * sizeof(float)));
    HIP_CHECK(hipMalloc(&device_b, n * sizeof(float)));
    HIP_CHECK(hipMalloc(&intermediate, n * sizeof(float)));
    HIP_CHECK(hipMalloc(&device_output, n * sizeof(float)));
    HIP_CHECK(hipStreamCreate(&stream));

    HIP_CHECK(hipMemcpy(device_input, input.data(), n * sizeof(float),
                        hipMemcpyHostToDevice));
    HIP_CHECK(hipMemcpy(device_b, b.data(), n * sizeof(float),
                        hipMemcpyHostToDevice));

    hipLaunchKernelGGL(scale_input, dim3(blocks), dim3(threads), 0, stream,
                       device_input, intermediate, n);
    HIP_CHECK(hipGetLastError());
    hipLaunchKernelGGL(combine, dim3(blocks), dim3(threads), 0, stream,
                       intermediate, device_b, device_output, n);
    HIP_CHECK(hipGetLastError());
    HIP_CHECK(hipDeviceSynchronize());

    HIP_CHECK(hipMemcpy(output.data(), device_output, n * sizeof(float),
                        hipMemcpyDeviceToHost));
    HIP_CHECK(hipStreamDestroy(stream));
    HIP_CHECK(hipFree(device_input));
    HIP_CHECK(hipFree(device_b));
    HIP_CHECK(hipFree(intermediate));
    HIP_CHECK(hipFree(device_output));

    int checksum = 0;
    for (int i = 0; i < n; ++i) {
        const float expected = static_cast<float>(5 * i + 1);
        if (std::fabs(output[i] - expected) > 0.001f) {
            std::cerr << "hip_kernel_chain: FAIL at " << i << " got "
                      << output[i] << " expected " << expected << "\n";
            return 2;
        }
        checksum += static_cast<int>(output[i]);
    }

    std::cout << "hip_kernel_chain: PASS checksum=" << checksum << "\n";
    return 0;
}
