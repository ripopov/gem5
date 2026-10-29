#ifndef GPU_LAB_MATMUL_MFMA_HH
#define GPU_LAB_MATMUL_MFMA_HH

#include "matmul.hh"

// FP32 16x16x4 MFMA: lane%16 selects an input row/column, lane/16
// selects K. Each lane owns four output rows at one output column.
// The two samples share this mapping and arithmetic; USE_LDS selects how
// operands reach the wave. The cache-only specialization allocates no LDS.
using F4 = float __attribute__((ext_vector_type(4)));
#ifndef USE_LDS
#define USE_LDS 1
#endif
#ifndef TILE_M
#define TILE_M 64
#endif
#ifndef TILE_N
#define TILE_N 64
#endif
#ifndef TILE_K
#define TILE_K 16
#endif
#ifndef WAVE_M
#define WAVE_M 32
#endif
#ifndef WAVE_N
#define WAVE_N 32
#endif
#ifndef PIPELINE
#define PIPELINE 2
#endif
#ifndef PRODUCERS
#define PRODUCERS 0
#endif
#ifndef GLOBAL_LOOKAHEAD
#define GLOBAL_LOOKAHEAD 1
#endif
#ifndef ACC_BARRIER
#define ACC_BARRIER 0
#endif
#ifndef STORE_VGPR
#define STORE_VGPR 1
#endif
#ifndef SWIZZLE
#define SWIZZLE 0
#endif
#ifndef LDS_PAD
#define LDS_PAD 0
#endif
#ifndef PREFETCH_LDS
// 0: load each fragment when needed; 1: preload the tile; 2: two fragments.
#define PREFETCH_LDS 0
#endif
constexpr int BM = TILE_M, BN = TILE_N, BK = TILE_K;
constexpr int WM = WAVE_M, WN = WAVE_N, MR = WM / 16, NR = WN / 16;
constexpr int workers = BM / WM * (BN / WN) * 64;
constexpr int loaders = PRODUCERS ? PRODUCERS : workers;
constexpr int LA = BM * BK / (loaders * 4), LB = BN * BK / (loaders * 4),
              U = BK / 4;
static_assert(BM % WM == 0 && BN % WN == 0 && BK % 4 == 0);
static_assert(WM % 16 == 0 && WN % 16 == 0);
static_assert(workers > 0 && workers + PRODUCERS <= 1024);
static_assert(PRODUCERS % 64 == 0 && PIPELINE >= 0 && PIPELINE <= 2);
static_assert(!SWIZZLE || ((BM & (BM - 1)) == 0 && (BN & (BN - 1)) == 0));
static_assert(!USE_LDS || (LA > 0 && LB > 0 && BM * BK % (loaders * 4) == 0 &&
                           BN * BK % (loaders * 4) == 0));
static_assert(!PRODUCERS || USE_LDS);
static_assert(GLOBAL_LOOKAHEAD == 1 ||
              (GLOBAL_LOOKAHEAD == 2 && USE_LDS && PIPELINE));
static_assert(PREFETCH_LDS >= 0 && PREFETCH_LDS <= 2);
static_assert(LDS_PAD >= 0 && LDS_PAD % 4 == 0);

__device__ int
ai(int row, int kk)
{
    return row ^
           (SWIZZLE ? ((((kk >> 2) & 3) * 8 ^ ((kk & 1) * 16)) & (BM - 1))
                    : 0);
}
__device__ int
bi(int col, int kk)
{
    return col ^ (SWIZZLE ? ((kk & 1) * 16 & (BN - 1)) : 0);
}
struct Tile
{
    float a[BK][BM + LDS_PAD];
    float b[BK][BN + LDS_PAD];
};
__device__ __forceinline__ void
fragment(const Tile &tile, int u, int row, int col, int lane, float (&av)[MR],
         float (&bv)[NR])
{
    const int kk = u * 4 + lane / 16;
#pragma unroll
    for (int r = 0; r < MR; ++r) {
        av[r] = tile.a[kk][ai(row + r * 16 + lane % 16, kk)];
    }
#pragma unroll
    for (int j = 0; j < NR; ++j) {
        bv[j] = tile.b[kk][bi(col + j * 16 + lane % 16, kk)];
    }
}
__device__ __forceinline__ void
fetch(const float *a, const float *b, int n, int k, int k0, int tid,
      F4 (&av)[LA], F4 (&bv)[LB])
{
#pragma unroll
    for (int i = 0; i < LA; ++i) {
        int v = tid + i * loaders;
        av[i] = *reinterpret_cast<const F4 *>(a + (v / (BK / 4)) * k + k0 +
                                              (v % (BK / 4)) * 4);
    }
#pragma unroll
    for (int i = 0; i < LB; ++i) {
        int v = tid + i * loaders;
        bv[i] = *reinterpret_cast<const F4 *>(b + (k0 + v / (BN / 4)) * n +
                                              (v % (BN / 4)) * 4);
    }
}
__device__ __forceinline__ void
publish(Tile &tile, int tid, const F4 (&av)[LA], const F4 (&bv)[LB])
{
#pragma unroll
    for (int i = 0; i < LA; ++i) {
        int v = tid + i * loaders, row = v / (BK / 4), kk = v % (BK / 4) * 4;
#pragma unroll
        for (int j = 0; j < 4; ++j) {
            tile.a[kk + j][ai(row, kk + j)] = av[i][j];
        }
    }
#pragma unroll
    for (int i = 0; i < LB; ++i) {
        int v = tid + i * loaders, kk = v / (BN / 4), col = v % (BN / 4) * 4;
        *reinterpret_cast<F4 *>(&tile.b[kk][bi(col, kk)]) = bv[i];
    }
}
__device__ __forceinline__ void
registers(const float *a, const float *b, int n, int k, int k0, int row,
          int col, int lane, float (&av)[U][MR], float (&bv)[U][NR])
{
#pragma unroll
    for (int u = 0; u < U; ++u) {
#pragma unroll
        for (int r = 0; r < MR; ++r) {
            av[u][r] =
                a[(row + r * 16 + lane % 16) * k + k0 + u * 4 + lane / 16];
        }
#pragma unroll
        for (int c = 0; c < NR; ++c) {
            bv[u][c] =
                b[(k0 + u * 4 + lane / 16) * n + col + c * 16 + lane % 16];
        }
    }
}
#if !defined(PERSISTENT) || !PERSISTENT
#ifndef WAVES_PER_EU
#define WAVES_PER_EU 0
#endif
#if WAVES_PER_EU
#define GEMM_BOUNDS __launch_bounds__(workers + PRODUCERS, WAVES_PER_EU)
#else
#define GEMM_BOUNDS __launch_bounds__(workers + PRODUCERS)
#endif
__global__ GEMM_BOUNDS void gemm(const float *__restrict__ a,
                                                 const float *__restrict__ b,
                                                 float *__restrict__ c, int n,
                                                 int k)
{
#if USE_LDS
    __shared__ Tile tiles[PIPELINE == 2 ? 2 : 1];
#endif
    int tid = threadIdx.x, lane = tid % 64, wave = tid / 64;
    int row = wave / (BN / WN) * WM, col = wave % (BN / WN) * WN;
    bool compute = PRODUCERS == 0 || tid < workers;
    bool loader = PRODUCERS == 0 || tid >= workers;
    int lt = PRODUCERS ? tid - workers : tid;
    a += blockIdx.y * BM * k;
    b += blockIdx.x * BN;
    c += blockIdx.y * BM * n + blockIdx.x * BN;
    F4 acc[MR][NR] = {};
#if USE_LDS
    F4 ga[GLOBAL_LOOKAHEAD][LA], gb[GLOBAL_LOOKAHEAD][LB];
    if (loader) {
        fetch(a, b, n, k, 0, lt, ga[0], gb[0]);
        publish(tiles[0], lt, ga[0], gb[0]);
        if (GLOBAL_LOOKAHEAD == 2 && BK < k) {
            fetch(a, b, n, k, BK, lt, ga[0], gb[0]);
        }
    }
    __syncthreads();
#else
    float ar[U][MR], br[U][NR], na[U][MR], nb[U][NR];
    if (PIPELINE) {
        registers(a, b, n, k, 0, row, col, lane, ar, br);
    }
#endif
    // Unroll the two register-buffer roles so future loads can remain
    // outstanding across an iteration boundary without register-copy waits.
    for (int base = 0, current = 0; base < k; base += GLOBAL_LOOKAHEAD * BK) {
#pragma unroll
        for (int phase = 0; phase < GLOBAL_LOOKAHEAD; ++phase) {
            const int k0 = base + phase * BK;
            if (k0 >= k) {
                continue;
            }
            bool next = k0 + BK < k;
            int target = PIPELINE == 2 ? current ^ 1 : 0;
#if USE_LDS
            if (PIPELINE && loader && next) {
                if (GLOBAL_LOOKAHEAD == 1) {
                    fetch(a, b, n, k, k0 + BK, lt, ga[phase], gb[phase]);
                } else if (k0 + 2 * BK < k) {
                    fetch(a, b, n, k, k0 + 2 * BK, lt, ga[phase ^ 1],
                          gb[phase ^ 1]);
                }
                if (PRODUCERS && PIPELINE == 2) {
                    publish(tiles[target], lt, ga[phase], gb[phase]);
                }
            }
#else
            if (PIPELINE && next) {
                registers(a, b, n, k, k0 + BK, row, col, lane, na, nb);
            }
            if (!PIPELINE) {
                registers(a, b, n, k, k0, row, col, lane, ar, br);
            }
#endif
            if (compute) {
#if USE_LDS && PREFETCH_LDS == 1
                // Fetch current operands before publishing the next LDS tile.
                // Later fragments may remain pending while earlier ones
                // compute.
                float fragmentsA[U][MR], fragmentsB[U][NR];
#pragma unroll
                for (int u = 0; u < U; ++u) {
                    fragment(tiles[current], u, row, col, lane, fragmentsA[u],
                             fragmentsB[u]);
                }
#elif USE_LDS && PREFETCH_LDS == 2
                float currentA[MR], currentB[NR], nextA[MR], nextB[NR];
                fragment(tiles[current], 0, row, col, lane, currentA,
                         currentB);
#endif
#pragma unroll
                for (int u = 0; u < U; ++u) {
#if USE_LDS
                    float ar[MR], br[NR];
#if PREFETCH_LDS == 1
#pragma unroll
                    for (int r = 0; r < MR; ++r) {
                        ar[r] = fragmentsA[u][r];
                    }
#pragma unroll
                    for (int j = 0; j < NR; ++j) {
                        br[j] = fragmentsB[u][j];
                    }
#elif PREFETCH_LDS == 2
#pragma unroll
                    for (int r = 0; r < MR; ++r) {
                        ar[r] = currentA[r];
                    }
#pragma unroll
                    for (int j = 0; j < NR; ++j) {
                        br[j] = currentB[j];
                    }
                    // Make current operands ready before issuing the next
                    // LDS reads, so their wait does not include later reads.
#pragma unroll
                    for (int r = 0; r < MR; ++r) {
                        asm volatile("" : : "v"(ar[r]));
                    }
#pragma unroll
                    for (int j = 0; j < NR; ++j) {
                        asm volatile("" : : "v"(br[j]));
                    }
                    __builtin_amdgcn_sched_barrier(0);
                    if (u + 1 < U) {
                        fragment(tiles[current], u + 1, row, col, lane, nextA,
                                 nextB);
                    }
#else
                    fragment(tiles[current], u, row, col, lane, ar, br);
#endif
                    if (PIPELINE == 2 && !PRODUCERS && u == U / 2 && next) {
                        publish(tiles[target], lt, ga[phase], gb[phase]);
                    }
#endif
#pragma unroll
                    for (int r = 0; r < MR; ++r) {
#pragma unroll
                        for (int j = 0; j < NR; ++j) {
#if USE_LDS
                            acc[r][j] = __builtin_amdgcn_mfma_f32_16x16x4f32(
                                ar[r], br[j], acc[r][j], 0, 0, 0);
#else
                            acc[r][j] = __builtin_amdgcn_mfma_f32_16x16x4f32(
                                ar[u][r], br[u][j], acc[r][j], 0, 0, 0);
#endif
#if ACC_BARRIER == 1
                            // Ablation: copy partial sums through VGPRs after
                            // each matrix operation instead of retaining
                            // AGPRs.
                            asm volatile("" : "+v"(acc[r][j]));
#elif ACC_BARRIER == 2
                            asm volatile("" : "+a"(acc[r][j]));
#endif
                        }
                    }
                    asm volatile("" ::: "memory");
                    __builtin_amdgcn_sched_barrier(0);
#if USE_LDS && PREFETCH_LDS == 2
                    if (u + 1 < U) {
#pragma unroll
                        for (int r = 0; r < MR; ++r) {
                            currentA[r] = nextA[r];
                        }
#pragma unroll
                        for (int j = 0; j < NR; ++j) {
                            currentB[j] = nextB[j];
                        }
                    }
#endif
                }
            }
#if USE_LDS
            if (PIPELINE != 2) {
                __syncthreads();
                if (loader && next) {
                    if (!PIPELINE) {
                        fetch(a, b, n, k, k0 + BK, lt, ga[phase], gb[phase]);
                    }
                    publish(tiles[0], lt, ga[phase], gb[phase]);
                }
            }
            __syncthreads();
            current = target;
#else
            if (PIPELINE && next) {
#pragma unroll
                for (int u = 0; u < U; ++u) {
#pragma unroll
                    for (int r = 0; r < MR; ++r) {
                        ar[u][r] = na[u][r];
                    }
#pragma unroll
                    for (int j = 0; j < NR; ++j) {
                        br[u][j] = nb[u][j];
                    }
                }
            }
#endif
        }
    }
    if (compute) {
#pragma unroll
        for (int r = 0; r < MR; ++r) {
#pragma unroll
            for (int j = 0; j < NR; ++j) {
#if STORE_VGPR
                // gem5 does not implement direct AGPR memory operands. Keep
                // partial sums in AGPRs, converting only the final outputs.
                asm volatile("" : "+v"(acc[r][j]));
#endif
#pragma unroll
                for (int v = 0; v < 4; ++v) {
                    c[(row + r * 16 + lane / 16 * 4 + v) * n + col + j * 16 +
                      lane % 16] = acc[r][j][v];
                }
            }
        }
    }
}
inline int
runMfma(int argc, char **argv, const char *name)
{
    std::cout << "MFMA tile=" << BM << 'x' << BN << 'x' << BK
              << " wave_tile=" << WM << 'x' << WN << " workers=" << workers
              << " producers=" << PRODUCERS << " lds=" << USE_LDS
              << " lookahead=" << GLOBAL_LOOKAHEAD << " pipeline=" << PIPELINE
              << " lds_pad=" << LDS_PAD << " swizzle=" << SWIZZLE
              << " prefetch_lds=" << PREFETCH_LDS
              << " accumulator_copy=" << ACC_BARRIER << '\n';
    return runMatmul(
        argc, argv, name,
        [](float *a, float *b, float *c, unsigned m, unsigned n, unsigned k) {
            hipLaunchKernelGGL(gemm, dim3(n / BN, m / BM),
                               dim3(workers + PRODUCERS), 0, 0, a, b, c, n, k);
        },
        BM, BN, BK);
}
#endif // !PERSISTENT

#endif
