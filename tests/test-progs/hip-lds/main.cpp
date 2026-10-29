#include <hip/hip_runtime.h>

#include <cstdint>
#include <cstdio>

using Pair = uint32_t __attribute__((ext_vector_type(2)));
using Quad = uint32_t __attribute__((ext_vector_type(4)));

__global__ void
ldsFootprints(uint32_t *out)
{
    __shared__ uint32_t data[4096];
    const unsigned lane = threadIdx.x;
    for (unsigned i = lane; i < 4096; i += 64) {
        data[i] = i * 17 + 3;
    }
    __syncthreads();
    const uint32_t addr = uint32_t(uintptr_t(data + lane * 4));
    uint32_t scalar;
    Pair dual, strided, wide;
    Quad quad;
    asm volatile("ds_read_b32 %0, %1 offset:20\n"
                 "s_waitcnt lgkmcnt(0)"
                 : "=v"(scalar)
                 : "v"(addr)
                 : "memory");
    asm volatile("ds_read2_b32 %0, %1 offset0:3 offset1:37\n"
                 "s_waitcnt lgkmcnt(0)"
                 : "=v"(dual)
                 : "v"(addr)
                 : "memory");
    asm volatile("ds_read2st64_b32 %0, %1 offset0:1 offset1:7\n"
                 "s_waitcnt lgkmcnt(0)"
                 : "=v"(strided)
                 : "v"(addr)
                 : "memory");
    asm volatile("ds_read_b64 %0, %1 offset:24\n"
                 "s_waitcnt lgkmcnt(0)"
                 : "=v"(wide)
                 : "v"(addr)
                 : "memory");
    asm volatile("ds_read_b128 %0, %1 offset:32\n"
                 "s_waitcnt lgkmcnt(0)"
                 : "=v"(quad)
                 : "v"(addr)
                 : "memory");
    out[lane * 16] = scalar;
    for (unsigned i = 0; i < 2; ++i) {
        out[lane * 16 + 1 + i] = dual[i];
        out[lane * 16 + 3 + i] = strided[i];
        out[lane * 16 + 5 + i] = wide[i];
    }
    for (unsigned i = 0; i < 4; ++i) {
        out[lane * 16 + 7 + i] = quad[i];
    }

    const uint32_t dest = uint32_t(uintptr_t(data + 1024 + lane * 4));
    Quad value = {lane, lane + 1, lane + 2, lane + 3};
    asm volatile("ds_write_b128 %0, %1 offset:16\n"
                 "s_waitcnt lgkmcnt(0)" ::"v"(dest),
                 "v"(value)
                 : "memory");
    __syncthreads();
    asm volatile("ds_read_b128 %0, %1 offset:16\n"
                 "s_waitcnt lgkmcnt(0)"
                 : "=v"(quad)
                 : "v"(dest)
                 : "memory");
    for (unsigned i = 0; i < 4; ++i) {
        out[lane * 16 + 11 + i] = quad[i];
    }
    if (lane % 3 != 0) {
        atomicAdd(data + 4095, 1U);
    }
    __syncthreads();
    out[lane * 16 + 15] = data[4095];
}

int
main()
{
    uint32_t *device;
    uint32_t output[64 * 16];
    if (hipMalloc(&device, sizeof(output)) != hipSuccess) {
        return 1;
    }
    hipLaunchKernelGGL(ldsFootprints, dim3(1), dim3(64), 0, 0, device);
    if (hipDeviceSynchronize() != hipSuccess ||
        hipMemcpy(output, device, sizeof(output), hipMemcpyDeviceToHost) !=
            hipSuccess ||
        hipFree(device) != hipSuccess) {
        return 1;
    }
    const unsigned offsets[] = {5, 3, 37, 64, 448, 6, 7, 8, 9, 10, 11};
    for (unsigned lane = 0; lane < 64; ++lane) {
        for (unsigned field = 0; field < 16; ++field) {
            uint32_t expected = field < 11
                                    ? (lane * 4 + offsets[field]) * 17 + 3
                                : field < 15 ? lane + field - 11
                                             : 4095 * 17 + 3 + 42;
            if (output[lane * 16 + field] != expected) {
                std::printf("FAIL lane=%u field=%u got=%u expected=%u\n", lane,
                            field, output[lane * 16 + field], expected);
                return 2;
            }
        }
    }
    std::puts("hip_lds: PASS");
    return 0;
}
