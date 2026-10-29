// Evaluation probes, separate from the student's naive starting point.
#include <hip/hip_runtime.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#define HIP_CHECK(call)                                                       \
    do {                                                                      \
        hipError_t status = (call);                                           \
        if (status != hipSuccess) {                                           \
            std::cerr << #call << ": " << hipGetErrorString(status) << '\n';  \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

constexpr unsigned M = 128, N = 256, K = 128;

__global__ void
tiled16(const float *a, const float *b, float *c)
{
    __shared__ float as[16][16], bs[16][16];
    unsigned x = threadIdx.x, y = threadIdx.y;
    unsigned row = blockIdx.y * 16 + y, col = blockIdx.x * 16 + x;
    float acc = 0;
    for (unsigned base = 0; base < K; base += 16) {
        as[y][x] = a[row * K + base + x];
        bs[y][x] = b[(base + y) * N + col];
        __syncthreads();
        for (unsigned k = 0; k < 16; ++k) {
            acc += as[y][k] * bs[k][x];
        }
        __syncthreads();
    }
    c[row * N + col] = acc;
}

__global__ void
register64(const float *a, const float *b, float *c)
{
    __shared__ float as[64][16], bs[16][64];
    unsigned x = threadIdx.x, y = threadIdx.y, tid = y * 16 + x;
    float acc[4][4] = {};
    for (unsigned base = 0; base < K; base += 16) {
        for (unsigned i = tid; i < 1024; i += 256) {
            as[i / 16][i % 16] =
                a[(blockIdx.y * 64 + i / 16) * K + base + i % 16];
            bs[i / 64][i % 64] =
                b[(base + i / 64) * N + blockIdx.x * 64 + i % 64];
        }
        __syncthreads();
        for (unsigned k = 0; k < 16; ++k) {
            float av[4], bv[4];
            for (unsigned i = 0; i < 4; ++i) {
                av[i] = as[y + i * 16][k];
                bv[i] = bs[k][x + i * 16];
            }
            for (unsigned i = 0; i < 4; ++i) {
                for (unsigned j = 0; j < 4; ++j) {
                    acc[i][j] += av[i] * bv[j];
                }
            }
        }
        __syncthreads();
    }
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned j = 0; j < 4; ++j) {
            c[(blockIdx.y * 64 + y + i * 16) * N + blockIdx.x * 64 + x +
              j * 16] = acc[i][j];
        }
    }
}

// A read-only pass substantially larger than the default shared L2. Vector
// loads and many independent workgroups provide memory-level parallelism.
__global__ void
stream(const float4 *a, float *out, unsigned count)
{
    unsigned tid = blockIdx.x * blockDim.x + threadIdx.x;
    unsigned step = gridDim.x * blockDim.x;
    float sum = 0;
    for (unsigned i = tid; i < count; i += step) {
        float4 v = a[i];
        sum += v.x + v.y + v.z + v.w;
    }
    out[tid] = sum;
}

__global__ void
lds(unsigned *out, unsigned stride)
{
    __shared__ volatile unsigned data[1024];
    unsigned tid = threadIdx.x;
    for (unsigned i = tid; i < 1024; i += blockDim.x) {
        data[i] = i % 8;
    }
    __syncthreads();
    unsigned sum = 0;
    for (unsigned k = 0; k < 128; ++k) {
        sum += data[(tid * stride + k) % 1024];
    }
    out[blockIdx.x * blockDim.x + tid] = sum;
}

int
main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr
            << "usage: eight-cu-probe tiled16|register64|stream|lds1|lds32\n";
        return 1;
    }
    std::string mode = argv[1];
    hipDeviceProp_t props{};
    HIP_CHECK(hipGetDeviceProperties(&props, 0));
    std::cout << "device: CU=" << props.multiProcessorCount
              << " wave=" << props.warpSize
              << " shared_per_block=" << props.sharedMemPerBlock
              << " global_bytes=" << props.totalGlobalMem << '\n';
    if (mode == "tiled16" || mode == "register64") {
        std::vector<float> a(M * K), b(K * N), c(M * N);
        for (unsigned r = 0; r < M; ++r) {
            for (unsigned k = 0; k < K; ++k) {
                a[r * K + k] = (r + k) % 8;
            }
        }
        for (unsigned k = 0; k < K; ++k) {
            for (unsigned col = 0; col < N; ++col) {
                b[k * N + col] = (3 * k + col) % 8;
            }
        }
        float *da, *db, *dc;
        HIP_CHECK(hipMalloc(&da, a.size() * 4));
        HIP_CHECK(hipMalloc(&db, b.size() * 4));
        HIP_CHECK(hipMalloc(&dc, c.size() * 4));
        HIP_CHECK(
            hipMemcpy(da, a.data(), a.size() * 4, hipMemcpyHostToDevice));
        HIP_CHECK(
            hipMemcpy(db, b.data(), b.size() * 4, hipMemcpyHostToDevice));
        if (mode == "tiled16") {
            hipLaunchKernelGGL(tiled16, dim3(N / 16, M / 16), dim3(16, 16), 0,
                               0, da, db, dc);
        } else {
            hipLaunchKernelGGL(register64, dim3(N / 64, M / 64), dim3(16, 16),
                               0, 0, da, db, dc);
        }
        HIP_CHECK(hipGetLastError());
        HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(
            hipMemcpy(c.data(), dc, c.size() * 4, hipMemcpyDeviceToHost));
        std::uint64_t checksum = 0;
        for (unsigned r = 0; r < M; ++r) {
            for (unsigned col = 0; col < N; ++col) {
                float expected = 0;
                for (unsigned k = 0; k < K; ++k) {
                    expected += a[r * K + k] * b[k * N + col];
                }
                if (c[r * N + col] != expected) {
                    return 2;
                }
                checksum += c[r * N + col];
            }
        }
        std::cout << "checksum=" << checksum << '\n';
        HIP_CHECK(hipFree(da));
        HIP_CHECK(hipFree(db));
        HIP_CHECK(hipFree(dc));
    } else if (mode == "stream") {
        constexpr unsigned count = 2 * 1024 * 1024; // 32 MiB of float4 input.
        constexpr unsigned threads = 128 * 256;
        std::vector<float4> a(count, make_float4(1, 2, 3, 4));
        std::vector<float> out(threads);
        float4 *da;
        float *dout;
        HIP_CHECK(hipMalloc(&da, a.size() * sizeof(float4)));
        HIP_CHECK(hipMalloc(&dout, out.size() * sizeof(float)));
        HIP_CHECK(hipMemcpy(da, a.data(), a.size() * sizeof(float4),
                            hipMemcpyHostToDevice));
        hipLaunchKernelGGL(stream, dim3(128), dim3(256), 0, 0, da, dout,
                           count);
        HIP_CHECK(hipGetLastError());
        HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipMemcpy(out.data(), dout, out.size() * 4,
                            hipMemcpyDeviceToHost));
        for (float got : out) {
            if (got != 10 * (count / threads)) {
                return 2;
            }
        }
        std::cout << "input_bytes=" << count * sizeof(float4) << '\n';
        HIP_CHECK(hipFree(da));
        HIP_CHECK(hipFree(dout));
    } else if (mode == "lds1" || mode == "lds32") {
        unsigned stride = mode == "lds1" ? 1 : 32;
        std::vector<unsigned> out(8 * 256);
        unsigned *dout;
        HIP_CHECK(hipMalloc(&dout, out.size() * sizeof(unsigned)));
        hipLaunchKernelGGL(lds, dim3(8), dim3(256), 0, 0, dout, stride);
        HIP_CHECK(hipGetLastError());
        HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipMemcpy(out.data(), dout, out.size() * 4,
                            hipMemcpyDeviceToHost));
        for (unsigned i = 0; i < out.size(); ++i) {
            unsigned expected = 0;
            for (unsigned k = 0; k < 128; ++k) {
                expected += ((i % 256) * stride + k) % 8;
            }
            if (out[i] != expected) {
                return 2;
            }
        }
        HIP_CHECK(hipFree(dout));
    } else {
        return 1;
    }
    std::cout << "eight-cu-probe: PASS " << mode << '\n';
}
