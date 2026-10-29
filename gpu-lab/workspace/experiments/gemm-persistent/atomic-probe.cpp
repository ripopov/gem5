// Each workgroup adds one to its own counter once from lane 0 of wave 0,
// with and without a returned value, and also writes a plain store. Any
// counter other than one indicates duplicated or lost global atomics.
#include <hip/hip_runtime.h>
#include <cstdio>
#include <vector>

__global__ void probe(unsigned *noRet, unsigned *withRet, unsigned *plain,
                      unsigned *retSum, int repeats)
{
    for (int r = 0; r < repeats; ++r) {
        if (threadIdx.x == 0) {
            atomicAdd(&noRet[blockIdx.x], 1u);
            unsigned old = atomicAdd(&withRet[blockIdx.x], 1u);
            retSum[blockIdx.x] += old;
            plain[blockIdx.x] += 1u;
        }
        __syncthreads();
    }
}

int main(int argc, char **argv)
{
    const int blocks = 8, threads = 256, repeats = argc > 1 ? atoi(argv[1]) : 4;
    std::vector<unsigned> zeros(blocks, 0), a(blocks), b(blocks), c(blocks), d(blocks);
    unsigned *da, *db, *dc, *dd;
    hipMalloc(&da, blocks * 4); hipMalloc(&db, blocks * 4);
    hipMalloc(&dc, blocks * 4); hipMalloc(&dd, blocks * 4);
    for (unsigned *p : {da, db, dc, dd})
        hipMemcpy(p, zeros.data(), blocks * 4, hipMemcpyHostToDevice);
    hipLaunchKernelGGL(probe, dim3(blocks), dim3(threads), 0, 0, da, db, dc, dd, repeats);
    hipDeviceSynchronize();
    hipMemcpy(a.data(), da, blocks * 4, hipMemcpyDeviceToHost);
    hipMemcpy(b.data(), db, blocks * 4, hipMemcpyDeviceToHost);
    hipMemcpy(c.data(), dc, blocks * 4, hipMemcpyDeviceToHost);
    hipMemcpy(d.data(), dd, blocks * 4, hipMemcpyDeviceToHost);
    bool ok = true;
    for (int i = 0; i < blocks; ++i) {
        printf("block %d: atomic_noret=%u atomic_ret=%u plain=%u ret_sum=%u\n",
               i, a[i], b[i], c[i], d[i]);
        ok &= a[i] == unsigned(repeats) && b[i] == unsigned(repeats) &&
              c[i] == unsigned(repeats);
    }
    printf("atomic_probe: %s repeats=%d\n", ok ? "PASS" : "FAIL", repeats);
    return ok ? 0 : 2;
}
