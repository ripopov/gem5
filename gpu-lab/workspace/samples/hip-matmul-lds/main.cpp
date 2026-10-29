// Use MATMUL_MFMA=0 to study the ordinary FP32 vector-ALU progression.
#ifndef MATMUL_MFMA
#define MATMUL_MFMA 1
#endif

#if MATMUL_MFMA
#define USE_LDS 1
// Selected configuration: 64x32 output tiles, one 64x16 MFMA wave tile per
// compute wave, and a persistent producer/consumer pipeline in which two
// producer waves stream K-stages through a four-slot LDS ring for two
// compute groups that alternate output tiles. See gpu-lab/GemmOptimization.md.
#ifndef TILE_N
#define TILE_N 32
#endif
#ifndef WAVE_M
#define WAVE_M 64
#endif
#ifndef WAVE_N
#define WAVE_N 16
#endif
#ifndef PIPELINE
#define PIPELINE 2
#endif
#ifndef PREFETCH_LDS
#define PREFETCH_LDS 2
#endif
#ifndef LDS_PAD
#define LDS_PAD 16
#endif
// PERSISTENT=0 restores the one-tile-per-workgroup kernel as a control.
#ifndef PERSISTENT
#define PERSISTENT 1
#endif
#if PERSISTENT
#ifndef PRODUCERS
#define PRODUCERS 128
#endif
#ifndef GROUPS
#define GROUPS 2
#endif
#ifndef STAGES
#define STAGES 4
#endif
#ifndef PREFETCH
#define PREFETCH 1
#endif
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
    return runPersistent(argc, argv, "hip_matmul_lds");
#else
    return runMfma(argc, argv, "hip_matmul_lds");
#endif
}
#else
#include "../common/matmul.hh"

using Float4 = float __attribute__((ext_vector_type(4)));

// Compile-time knobs are kept explicit for reproducible tuning experiments.
#ifndef TILE_M
#define TILE_M 64
#endif
#ifndef TILE_N
#define TILE_N 64
#endif
#ifndef TILE_K
#define TILE_K 16
#endif
#ifndef REG_M
#define REG_M 4
#endif
#ifndef REG_N
#define REG_N 4
#endif
#ifndef PIPELINE
#define PIPELINE 2
#endif
#ifndef PRODUCERS
#define PRODUCERS 0
#endif
#ifndef LDS_PAD
#define LDS_PAD 0
#endif
#ifndef PUBLISH_K
#define PUBLISH_K (TILE_K / 2)
#endif
#ifndef SCHEDULE_FRAGMENTS
#define SCHEDULE_FRAGMENTS 1
#endif

constexpr int BM = TILE_M, BN = TILE_N, BK = TILE_K;
constexpr int RM = REG_M, RN = REG_N;
constexpr int workers = BM / RM * (BN / RN);
constexpr int loaders = PRODUCERS ? PRODUCERS : workers;
constexpr int loadA = BM * BK / (4 * loaders);
constexpr int loadB = BK * BN / (4 * loaders);
constexpr int buffers = PIPELINE == 2 ? 2 : 1;
static_assert(BM % RM == 0 && BN % RN == 0 && RN == 4);
static_assert(BK % 4 == 0 && BM * BK % (4 * loaders) == 0);
static_assert(BK * BN % (4 * loaders) == 0);
static_assert(workers % 64 == 0 && workers + PRODUCERS <= 1024);
static_assert(PRODUCERS == 0 || (PRODUCERS % 64 == 0 && PIPELINE == 2));
static_assert(PUBLISH_K >= 0 && PUBLISH_K < BK);

struct Tile
{
    float a[BK][BM + LDS_PAD];
    float b[BK][BN];
};

__device__ __forceinline__ void
prefetch(const float *a, const float *b, int n, int k, int k0, int tid,
         Float4 (&ar)[loadA], Float4 (&br)[loadB])
{
#pragma unroll
    for (int i = 0; i < loadA; ++i) {
        int v = tid + i * loaders;
        ar[i] = *reinterpret_cast<const Float4 *>(a + (v / (BK / 4)) * k + k0 +
                                                  (v % (BK / 4)) * 4);
    }
#pragma unroll
    for (int i = 0; i < loadB; ++i) {
        int v = tid + i * loaders;
        br[i] = *reinterpret_cast<const Float4 *>(b + (k0 + v / (BN / 4)) * n +
                                                  (v % (BN / 4)) * 4);
    }
}

__device__ __forceinline__ void
publish(Tile &tile, int tid, const Float4 (&ar)[loadA],
        const Float4 (&br)[loadB])
{
#pragma unroll
    for (int i = 0; i < loadA; ++i) {
        int v = tid + i * loaders;
        int row = v / (BK / 4), col = v % (BK / 4) * 4;
        tile.a[col][row] = ar[i].x;
        tile.a[col + 1][row] = ar[i].y;
        tile.a[col + 2][row] = ar[i].z;
        tile.a[col + 3][row] = ar[i].w;
    }
#pragma unroll
    for (int i = 0; i < loadB; ++i) {
        reinterpret_cast<Float4 *>(tile.b)[tid + i * loaders] = br[i];
    }
}

__device__ __forceinline__ void
fragment(const Tile &tile, int kk, int row, int col, float (&ar)[RM],
         Float4 &br)
{
#pragma unroll
    for (int r = 0; r < RM; ++r) {
        ar[r] = tile.a[kk][row + r * (BM / RM)];
    }
    br = *reinterpret_cast<const Float4 *>(&tile.b[kk][col]);
}

__global__
__launch_bounds__(workers +
                  PRODUCERS) void matmulLds(const float *__restrict__ a,
                                            const float *__restrict__ b,
                                            float *__restrict__ c, int n,
                                            int k)
{
    __shared__ Tile tiles[buffers];
    const int tid = threadIdx.x;
    const bool loader = PRODUCERS == 0 || tid >= workers;
    const bool compute = PRODUCERS == 0 || tid < workers;
    const int loadTid = PRODUCERS ? tid - workers : tid;
    const int row = tid / (BN / RN), col = tid % (BN / RN) * RN;
    a += blockIdx.y * BM * k;
    b += blockIdx.x * BN;
    c += blockIdx.y * BM * n + blockIdx.x * BN;
    Float4 nextA[loadA], nextB[loadB];
    float acc[RM][RN] = {};
    if (loader) {
        prefetch(a, b, n, k, 0, loadTid, nextA, nextB);
        publish(tiles[0], loadTid, nextA, nextB);
    }
    __syncthreads();
    for (int k0 = 0, current = 0; k0 < k; k0 += BK) {
        const bool next = k0 + BK < k;
        const int target = PIPELINE == 2 ? current ^ 1 : 0;
        if (PIPELINE && loader && next) {
            prefetch(a, b, n, k, k0 + BK, loadTid, nextA, nextB);
            // Preserve the early load position without waiting for the data.
            __builtin_amdgcn_sched_barrier(0);
            if (PRODUCERS) {
                publish(tiles[target], loadTid, nextA, nextB);
            }
        }
        if (compute) {
            float ar[2][RM];
            Float4 br[2];
            fragment(tiles[current], 0, row, col, ar[0], br[0]);
#pragma unroll
            for (int kk = 0; kk < BK; ++kk) {
                const int slot = kk % 2;
                if (kk + 1 < BK) {
                    fragment(tiles[current], kk + 1, row, col, ar[slot ^ 1],
                             br[slot ^ 1]);
                }
                if (PIPELINE == 2 && PRODUCERS == 0 && kk == PUBLISH_K &&
                    next) {
                    publish(tiles[target], loadTid, nextA, nextB);
                }
#pragma unroll
                for (int r = 0; r < RM; ++r) {
                    acc[r][0] += ar[slot][r] * br[slot].x;
                    acc[r][1] += ar[slot][r] * br[slot].y;
                    acc[r][2] += ar[slot][r] * br[slot].z;
                    acc[r][3] += ar[slot][r] * br[slot].w;
                    asm volatile(""
                                 : "+v"(acc[r][0]), "+v"(acc[r][1]),
                                   "+v"(acc[r][2]), "+v"(acc[r][3]));
                }
                if (SCHEDULE_FRAGMENTS) {
                    asm volatile("" ::: "memory");
                    __builtin_amdgcn_sched_barrier(0);
                }
            }
        }
        if (PIPELINE != 2) {
            // Single buffering must protect consumers before overwriting LDS.
            __syncthreads();
            if (next) {
                if (PIPELINE == 0) {
                    prefetch(a, b, n, k, k0 + BK, loadTid, nextA, nextB);
                }
                publish(tiles[0], loadTid, nextA, nextB);
            }
        }
        __syncthreads();
        current = target;
    }
    if (compute) {
#pragma unroll
        for (int r = 0; r < RM; ++r) {
            Float4 value = {acc[r][0], acc[r][1], acc[r][2], acc[r][3]};
            *reinterpret_cast<Float4 *>(c + (row + r * (BM / RM)) * n + col) =
                value;
        }
    }
}

int
main(int argc, char **argv)
{
    std::cout << "LDS tile=" << BM << 'x' << BN << 'x' << BK << " reg=" << RM
              << 'x' << RN << " workers=" << workers
              << " producers=" << PRODUCERS << " pipeline=" << PIPELINE
              << " lds_bytes=" << sizeof(Tile) * buffers << '\n';
    return runMatmul(
        argc, argv, "hip_matmul_lds",
        [](float *a, float *b, float *c, unsigned m, unsigned n, unsigned k) {
            hipLaunchKernelGGL(matmulLds, dim3(n / BN, m / BM),
                               dim3(workers + PRODUCERS), 0, 0, a, b, c, n, k);
        },
        BM, BN, BK);
}

#endif
