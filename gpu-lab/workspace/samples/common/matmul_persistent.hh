#ifndef GPU_LAB_MATMUL_PERSISTENT_HH
#define GPU_LAB_MATMUL_PERSISTENT_HH

// Persistent GEMM: a fixed number of workgroups each walk a contiguous run
// of output tiles, so operand staging for one tile can overlap the previous
// tile's output stores. Three organisations share the same MFMA mapping,
// tile geometry and host contract as the nonpersistent kernel in
// matmul_mfma.hh:
//
//  PRODUCERS == 0, USE_LDS == 1  cooperative: every wave loads operands into
//                                LDS and computes; S_BARRIER separates the
//                                STAGES (1 or 2) LDS buffers.
//  PRODUCERS  > 0, USE_LDS == 1  specialised: producer waves stream K-stages
//                                through a STAGES-deep LDS ring, holding
//                                PREFETCH stages of global loads in flight;
//                                GROUPS compute groups take alternate output
//                                tiles, keep accumulators in AGPRs and store
//                                their own results. Waves synchronise with
//                                the LDS counters in wg_pipeline.hh only.
//  USE_LDS == 0                  cache/register only: each wave loads its own
//                                fragments; PIPELINE=1 prefetches the next
//                                stage, including across tile boundaries.
#include "matmul_mfma.hh"
#include "wg_pipeline.hh"

#ifndef GROUPS
#define GROUPS 1
#endif
#ifndef STAGES
#define STAGES 2
#endif
#ifndef PREFETCH
#define PREFETCH 1
#endif
#ifndef WORKGROUPS
// Persistent workgroups. Eight CUs are modeled; the guest-visible topology
// is not used because full-system reports the physical device.
#define WORKGROUPS 8
#endif
#ifndef ORDER
// 0: contiguous tile runs per workgroup, 1: strided round robin.
#define ORDER 0
#endif
#ifndef WAVES_PER_EU
// Minimum waves per SIMD the compiler must allow, which caps its register
// allocation (and may spill). 0 leaves the compiler's own choice.
#define WAVES_PER_EU 0
#endif
#ifndef PUBLISH_MID
// Cooperative double buffering publishes the next stage in the middle of the
// current stage's computation (1) or after it (0).
#define PUBLISH_MID 1
#endif

constexpr int computeWaves = workers / 64;
constexpr int producerWaves = PRODUCERS / 64;
constexpr int computeThreads = workers * GROUPS;
constexpr int totalThreads = computeThreads + PRODUCERS;
static_assert(totalThreads <= 1024);
static_assert(GROUPS >= 1 && (PRODUCERS > 0 || GROUPS == 1));
static_assert(STAGES >= 1 && (PRODUCERS == 0 ? STAGES <= 2 : STAGES >= 2));
static_assert(PREFETCH >= 1 && PREFETCH <= 4);
static_assert(USE_LDS || PRODUCERS == 0);
static_assert(WORKGROUPS >= 1);

#if USE_LDS
struct Ring
{
    Tile stage[STAGES];
    unsigned full[STAGES];
    unsigned empty[STAGES];
};
constexpr size_t ldsBytes = sizeof(Ring);
#else
constexpr size_t ldsBytes = 0;
#endif

// One K-stage of MFMA work from an LDS tile. `mid` runs halfway through, where
// cooperative double buffering publishes the next stage.
template <class Mid>
__device__ __forceinline__ void
computeStage(const Tile &tile, int row, int col, int lane,
             F4 (&acc)[MR][NR], Mid mid)
{
#if PREFETCH_LDS == 1
    float fragmentsA[U][MR], fragmentsB[U][NR];
#pragma unroll
    for (int u = 0; u < U; ++u) {
        fragment(tile, u, row, col, lane, fragmentsA[u], fragmentsB[u]);
    }
#elif PREFETCH_LDS == 2
    float currentA[MR], currentB[NR], nextA[MR], nextB[NR];
    fragment(tile, 0, row, col, lane, currentA, currentB);
#endif
#pragma unroll
    for (int u = 0; u < U; ++u) {
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
            fragment(tile, u + 1, row, col, lane, nextA, nextB);
        }
#else
        fragment(tile, u, row, col, lane, ar, br);
#endif
        if (u == U / 2) {
            mid();
        }
#pragma unroll
        for (int r = 0; r < MR; ++r) {
#pragma unroll
            for (int j = 0; j < NR; ++j) {
                acc[r][j] = __builtin_amdgcn_mfma_f32_16x16x4f32(
                    ar[r], br[j], acc[r][j], 0, 0, 0);
            }
        }
        asm volatile("" ::: "memory");
        __builtin_amdgcn_sched_barrier(0);
#if PREFETCH_LDS == 2
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

__device__ __forceinline__ void
computeRegisters(const float (&ar)[U][MR], const float (&br)[U][NR],
                 F4 (&acc)[MR][NR])
{
#pragma unroll
    for (int u = 0; u < U; ++u) {
#pragma unroll
        for (int r = 0; r < MR; ++r) {
#pragma unroll
            for (int j = 0; j < NR; ++j) {
                acc[r][j] = __builtin_amdgcn_mfma_f32_16x16x4f32(
                    ar[u][r], br[u][j], acc[r][j], 0, 0, 0);
            }
        }
        asm volatile("" ::: "memory");
        __builtin_amdgcn_sched_barrier(0);
    }
}

__device__ __forceinline__ void
storeTile(float *c, int n, int row, int col, int lane, F4 (&acc)[MR][NR])
{
#pragma unroll
    for (int r = 0; r < MR; ++r) {
#pragma unroll
        for (int j = 0; j < NR; ++j) {
#if STORE_VGPR
            // The simulator has no direct AGPR memory operands: keep partial
            // sums in AGPRs and move only final outputs to VGPRs.
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

// Walks (tile, K-stage) pairs in the order the workgroup processes them.
struct StageCursor
{
    int tile, stage;
    __device__ __forceinline__ void
    advance(int ks)
    {
        if (++stage == ks) {
            stage = 0;
            ++tile;
        }
    }
};

#if WAVES_PER_EU
#define PERSISTENT_BOUNDS __launch_bounds__(totalThreads, WAVES_PER_EU)
#else
#define PERSISTENT_BOUNDS __launch_bounds__(totalThreads)
#endif
__global__ PERSISTENT_BOUNDS void gemmPersistent(
    const float *__restrict__ a, const float *__restrict__ b,
    float *__restrict__ c, int n, int k, int tilesN, int tiles,
    unsigned *__restrict__ tileCounts)
{
#if USE_LDS
    __shared__ Ring ring;
#endif
    const int tid = threadIdx.x, lane = tid % 64, wave = tid / 64;
    const int ks = k / BK;
    int first, count, stride;
    if (ORDER == 0) {
        first = blockIdx.x * tiles / gridDim.x;
        count = (blockIdx.x + 1) * tiles / gridDim.x - first;
        stride = 1;
    } else {
        first = blockIdx.x;
        count = (tiles - first + gridDim.x - 1) / gridDim.x;
        stride = gridDim.x;
    }
    const int stages = count * ks;
    auto origin = [&](int localTile, int &tm, int &tn) {
        const int t = first + localTile * stride;
        tm = t / tilesN;
        tn = t - tm * tilesN;
    };
#if USE_LDS
    if (tid < STAGES) {
        ring.full[tid] = 0;
        ring.empty[tid] = 0;
    }
    __syncthreads();
#endif

#if PRODUCERS
    if (wave >= computeWaves * GROUPS) {
        const int lt = tid - computeThreads;
        F4 ga[PREFETCH][LA], gb[PREFETCH][LB];
        StageCursor next = {0, 0};
        auto fetchStage = [&](F4 (&av)[LA], F4 (&bv)[LB]) {
            int tm, tn;
            origin(next.tile, tm, tn);
            fetch(a + tm * BM * k, b + tn * BN, n, k, next.stage * BK, lt, av,
                  bv);
            next.advance(ks);
        };
#pragma unroll
        for (int p = 0; p < PREFETCH; ++p) {
            if (p < stages) {
                fetchStage(ga[p], gb[p]);
            }
        }
        int slot = 0;
        unsigned fill = 0;
        for (int base = 0; base < stages; base += PREFETCH) {
#pragma unroll
            for (int phase = 0; phase < PREFETCH; ++phase) {
                const int q = base + phase;
                if (q < stages) {
                    if (fill) {
                        pipelineWait(&ring.empty[slot], fill * computeWaves);
                    }
                    publish(ring.stage[slot], lt, ga[phase], gb[phase]);
                    pipelineSignal(&ring.full[slot], lane);
                    if (q + PREFETCH < stages) {
                        fetchStage(ga[phase], gb[phase]);
                    }
                    if (++slot == STAGES) {
                        slot = 0;
                        ++fill;
                    }
                }
            }
        }
        return;
    }
#endif

    const int g = wave / computeWaves, cw = wave - g * computeWaves;
    const int row = cw / (BN / WN) * WM, col = cw % (BN / WN) * WN;
    // Work-distribution evidence: each group's first wave counts the tiles
    // it stored and publishes the count once at exit. Global atomics are
    // avoided; see experiments/gemm-persistent/atomic-probe.cpp.
    unsigned finished = 0;
    auto finishTile = [&](int tm, int tn, F4 (&acc)[MR][NR]) {
        storeTile(c + tm * BM * n + tn * BN, n, row, col, lane, acc);
        ++finished;
    };

#if USE_LDS && PRODUCERS
    // Stages are numbered tile-major; this group consumes every GROUPS-th
    // tile and skips the others' stages when advancing its ring position.
    int slot = (g * ks) % STAGES;
    unsigned fill = (g * ks) / STAGES;
    const int skipFill = (GROUPS - 1) * ks / STAGES;
    const int skipSlot = (GROUPS - 1) * ks % STAGES;
    for (int i = g; i < count; i += GROUPS) {
        int tm, tn;
        origin(i, tm, tn);
        F4 acc[MR][NR] = {};
        for (int j = 0; j < ks; ++j) {
            pipelineWait(&ring.full[slot], (fill + 1) * producerWaves);
            computeStage(ring.stage[slot], row, col, lane, acc, [] {});
            pipelineSignal(&ring.empty[slot], lane);
            if (++slot == STAGES) {
                slot = 0;
                ++fill;
            }
        }
        fill += skipFill;
        slot += skipSlot;
        if (slot >= STAGES) {
            slot -= STAGES;
            ++fill;
        }
        finishTile(tm, tn, acc);
    }
#elif USE_LDS
    F4 ga[LA], gb[LB];
    StageCursor next = {0, 0};
    auto fetchStage = [&] {
        int tm, tn;
        origin(next.tile, tm, tn);
        fetch(a + tm * BM * k, b + tn * BN, n, k, next.stage * BK, tid, ga,
              gb);
        next.advance(ks);
    };
    if (stages) {
        fetchStage();
        publish(ring.stage[0], tid, ga, gb);
    }
    __syncthreads();
    int slot = 0, q = 0;
    for (int i = 0; i < count; ++i) {
        int tm, tn;
        origin(i, tm, tn);
        F4 acc[MR][NR] = {};
        for (int j = 0; j < ks; ++j, ++q) {
            const bool more = q + 1 < stages;
            const int target = STAGES == 2 ? slot ^ 1 : 0;
            if (more) {
                fetchStage();
            }
            computeStage(ring.stage[slot], row, col, lane, acc, [&] {
                if (STAGES == 2 && PUBLISH_MID && more) {
                    publish(ring.stage[target], tid, ga, gb);
                }
            });
            if (STAGES == 2 && !PUBLISH_MID && more) {
                publish(ring.stage[target], tid, ga, gb);
            }
            if (STAGES == 1) {
                __syncthreads();
                if (more) {
                    publish(ring.stage[0], tid, ga, gb);
                }
            }
            __syncthreads();
            slot = target;
        }
        finishTile(tm, tn, acc);
    }
#else
    float ar[U][MR], br[U][NR], na[U][MR], nb[U][NR];
    StageCursor next = {0, 0};
    auto loadStage = [&](float (&av)[U][MR], float (&bv)[U][NR]) {
        int tm, tn;
        origin(next.tile, tm, tn);
        registers(a + tm * BM * k, b + tn * BN, n, k, next.stage * BK, row,
                  col, lane, av, bv);
        next.advance(ks);
    };
    if (PIPELINE && stages) {
        loadStage(ar, br);
    }
    int q = 0;
    for (int i = 0; i < count; ++i) {
        int tm, tn;
        origin(i, tm, tn);
        F4 acc[MR][NR] = {};
        for (int j = 0; j < ks; ++j, ++q) {
            const bool more = q + 1 < stages;
            if (PIPELINE) {
                if (more) {
                    loadStage(na, nb);
                    __builtin_amdgcn_sched_barrier(0);
                }
            } else {
                loadStage(ar, br);
            }
            computeRegisters(ar, br, acc);
            if (PIPELINE && more) {
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
        }
        finishTile(tm, tn, acc);
    }
#endif
    if (cw == 0 && lane == 0) {
        tileCounts[blockIdx.x * GROUPS + g] = finished;
    }
}

inline bool tileCountMismatch = false;

inline int
runPersistent(int argc, char **argv, const char *name)
{
    std::cout << "Persistent tile=" << BM << 'x' << BN << 'x' << BK
              << " wave_tile=" << WM << 'x' << WN << " groups=" << GROUPS
              << " compute_waves=" << computeWaves
              << " producers=" << PRODUCERS << " lds=" << USE_LDS
              << " stages=" << STAGES << " prefetch=" << PREFETCH
              << " prefetch_lds=" << PREFETCH_LDS << " pipeline=" << PIPELINE
              << " lds_pad=" << LDS_PAD << " swizzle=" << SWIZZLE
              << " workgroups=" << WORKGROUPS << " order=" << ORDER
              << " spin_sleep=" << SPIN_SLEEP
              << " waves_per_eu=" << WAVES_PER_EU
              << " lds_bytes=" << ldsBytes << '\n';
    const int status = runMatmul(
        argc, argv, name,
        [](float *a, float *b, float *c, unsigned m, unsigned n, unsigned k) {
            const int tilesN = n / BN, tiles = m / BM * tilesN;
            const int groups = tiles < WORKGROUPS ? tiles : WORKGROUPS;
            // Every slot is written exactly once by the kernel, so no
            // initialisation copy is needed; the single copy back is a
            // small runtime blit kernel outside the GEMM's own work.
            std::vector<unsigned> counts(size_t(groups) * GROUPS, 0);
            unsigned *deviceCounts;
            hipCheck(
                hipMalloc(&deviceCounts, counts.size() * sizeof(unsigned)));
            hipLaunchKernelGGL(gemmPersistent, dim3(groups),
                               dim3(totalThreads), 0, 0, a, b, c, n, k,
                               tilesN, tiles, deviceCounts);
            hipCheck(hipGetLastError());
            hipCheck(hipDeviceSynchronize());
            hipCheck(hipMemcpy(counts.data(), deviceCounts,
                               counts.size() * sizeof(unsigned),
                               hipMemcpyDeviceToHost));
            hipCheck(hipFree(deviceCounts));
            unsigned least = ~0u, most = 0, total = 0;
            std::cout << "tiles_per_workgroup=";
            for (int w = 0; w < groups; ++w) {
                unsigned value = 0;
                for (int g = 0; g < GROUPS; ++g) {
                    value += counts[w * GROUPS + g];
                }
                std::cout << value << ',';
                least = value < least ? value : least;
                most = value > most ? value : most;
                total += value;
            }
            std::cout << " min=" << least << " max=" << most
                      << " total=" << total << " expected=" << tiles << '\n';
            tileCountMismatch = total != unsigned(tiles);
        },
        BM, BN, BK);
    if (status == 0 && tileCountMismatch) {
        std::cerr << name << ": tile count mismatch\n";
        return 3;
    }
    return status;
}

#endif
