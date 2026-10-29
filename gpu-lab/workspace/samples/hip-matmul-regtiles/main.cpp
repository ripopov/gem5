// Use MATMUL_MFMA=0 to study the ordinary FP32 vector-ALU progression.
#ifndef MATMUL_MFMA
#define MATMUL_MFMA 1
#endif

#if MATMUL_MFMA
#define USE_LDS 0
// Selected configuration: 64x32 output tiles of two 32x32 MFMA wave tiles,
// each wave prefetching its next K-stage from the caches while computing,
// walked persistently by eight workgroups. See gpu-lab/GemmOptimization.md.
#ifndef TILE_N
#define TILE_N 32
#endif
#ifndef WAVE_M
#define WAVE_M 32
#endif
#ifndef WAVE_N
#define WAVE_N 32
#endif
#ifndef PIPELINE
#define PIPELINE 1
#endif
// PERSISTENT=0 restores the one-tile-per-workgroup kernel as a control.
#ifndef PERSISTENT
#define PERSISTENT 1
#endif
#if PERSISTENT
#include "../common/matmul_persistent.hh"
#else
#include "../common/matmul_mfma.hh"
#endif

int
main(int argc, char **argv)
{
#if PERSISTENT
    return runPersistent(argc, argv, "hip_matmul_regtiles");
#else
    return runMfma(argc, argv, "hip_matmul_regtiles");
#endif
}
#else
#include "../common/matmul.hh"

#ifndef TILE_M
#define TILE_M 32
#endif
#ifndef TILE_N
#define TILE_N 64
#endif
#ifndef REG_M
#define REG_M 4
#endif
#ifndef REG_N
#define REG_N 4
#endif
#ifndef K_UNROLL
#define K_UNROLL 4
#endif
#ifndef PREFETCH
#define PREFETCH 0
#endif

using BVector = float __attribute__((ext_vector_type(REG_N)));

constexpr int BM = TILE_M, BN = TILE_N, RM = REG_M, RN = REG_N;
constexpr int U = K_UNROLL, threads = BM / RM * (BN / RN);
static_assert(BM % RM == 0 && BN % RN == 0 && (RN == 4 || RN == 8));
static_assert(threads % 64 == 0 && threads <= 1024);

__device__ __forceinline__ void
load(const float *a, const float *b, int n, int k, int k0, float (&ar)[U][RM],
     BVector (&br)[U])
{
#pragma unroll
    for (int u = 0; u < U; ++u) {
#pragma unroll
        for (int r = 0; r < RM; ++r) {
            ar[u][r] = a[r * (BM / RM) * k + k0 + u];
        }
        br[u] = *reinterpret_cast<const BVector *>(b + (k0 + u) * n);
    }
}

__global__
__launch_bounds__(threads) void matmulRegtiles(const float *__restrict__ a,
                                               const float *__restrict__ b,
                                               float *__restrict__ c, int n,
                                               int k)
{
    const int row = blockIdx.y * BM + threadIdx.x / (BN / RN);
    const int col = blockIdx.x * BN + threadIdx.x % (BN / RN) * RN;
    a += row * k;
    b += col;
    float acc[RM][RN] = {};
    float ar[U][RM], nextAr[U][RM];
    BVector br[U], nextBr[U];
    if (PREFETCH) {
        load(a, b, n, k, 0, ar, br);
    }
    for (int k0 = 0; k0 < k; k0 += U) {
        if (PREFETCH) {
            if (k0 + U < k) {
                load(a, b, n, k, k0 + U, nextAr, nextBr);
                __builtin_amdgcn_sched_barrier(0);
            }
        } else {
            load(a, b, n, k, k0, ar, br);
        }
#pragma unroll
        for (int u = 0; u < U; ++u) {
#pragma unroll
            for (int r = 0; r < RM; ++r) {
#if REG_N == 4
                acc[r][0] += ar[u][r] * br[u].x;
                acc[r][1] += ar[u][r] * br[u].y;
                acc[r][2] += ar[u][r] * br[u].z;
                acc[r][3] += ar[u][r] * br[u].w;
#else
#pragma unroll
                for (int j = 0; j < RN; ++j) {
                    acc[r][j] += ar[u][r] * br[u][j];
                }
#endif
            }
        }
        if (PREFETCH && k0 + U < k) {
#pragma unroll
            for (int u = 0; u < U; ++u) {
#pragma unroll
                for (int r = 0; r < RM; ++r) {
                    ar[u][r] = nextAr[u][r];
                }
                br[u] = nextBr[u];
            }
        }
    }
#pragma unroll
    for (int r = 0; r < RM; ++r) {
        BVector value = {acc[r][0], acc[r][1], acc[r][2], acc[r][3]};
#if REG_N == 8
#pragma unroll
        for (int j = 4; j < RN; ++j) {
            value[j] = acc[r][j];
        }
#endif
        *reinterpret_cast<BVector *>(c + (row + r * (BM / RM)) * n + col) =
            value;
    }
}

int
main(int argc, char **argv)
{
    std::cout << "Registers tile=" << BM << 'x' << BN << " reg=" << RM << 'x'
              << RN << " threads=" << threads << " unroll=" << U
              << " prefetch=" << PREFETCH << '\n';
    return runMatmul(
        argc, argv, "hip_matmul_regtiles",
        [](float *a, float *b, float *c, unsigned m, unsigned n, unsigned k) {
            hipLaunchKernelGGL(matmulRegtiles, dim3(n / BN, m / BM),
                               dim3(threads), 0, 0, a, b, c, n, k);
        },
        BM, BN, U);
}

#endif
