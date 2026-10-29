// Stress test for the LDS producer/consumer ring in samples/common/
// wg_pipeline.hh, independent of GEMM. Producer waves fill a STAGES-deep
// ring with stage-specific patterns; GROUPS consumer groups of GROUP_WAVES
// waves each read whole stages in tile-major runs of KS stages, checking
// every element and accumulating a checksum. Timing is perturbed with
// per-stage pseudo-random s_sleep delays so that different interleavings are
// exercised. BROKEN=1 removes the consumer-side waits to show that the
// checks detect a protocol violation.
#include <hip/hip_runtime.h>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

#include "../../samples/common/wg_pipeline.hh"

#ifndef PRODUCER_WAVES
#define PRODUCER_WAVES 1
#endif
#ifndef GROUPS
#define GROUPS 2
#endif
#ifndef GROUP_WAVES
#define GROUP_WAVES 4
#endif
#ifndef STAGES
#define STAGES 3
#endif
#ifndef PAYLOAD
#define PAYLOAD 2048
#endif
#ifndef JITTER
#define JITTER 8
#endif
#ifndef BROKEN
#define BROKEN 0
#endif

constexpr int consumerWaves = GROUPS * GROUP_WAVES;
constexpr int totalWaves = consumerWaves + PRODUCER_WAVES;
constexpr int perLaneProduce = PAYLOAD / (PRODUCER_WAVES * 64);
constexpr int perLaneConsume = PAYLOAD / 64;
static_assert(PAYLOAD % (PRODUCER_WAVES * 64) == 0 && PAYLOAD % 64 == 0);
static_assert(totalWaves * 64 <= 1024);

struct Ring
{
    float stage[STAGES][PAYLOAD];
    unsigned full[STAGES];
    unsigned empty[STAGES];
};

__device__ __forceinline__ float
pattern(unsigned q, int index)
{
    return float((q * 7u + unsigned(index) * 3u) % 251u);
}

__device__ __forceinline__ unsigned
hash(unsigned q, unsigned wave, unsigned block)
{
    unsigned h = q * 2654435761u ^ wave * 40503u ^ block * 97u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

__device__ __forceinline__ void
jitter(unsigned q, unsigned wave, unsigned salt)
{
#if JITTER > 0
    const unsigned delay = hash(q, wave, blockIdx.x + salt) % (JITTER + 1);
    // s_sleep needs a compile-time immediate; select among small delays.
    switch (delay) {
      case 1: __builtin_amdgcn_s_sleep(1); break;
      case 2: __builtin_amdgcn_s_sleep(2); break;
      case 3: __builtin_amdgcn_s_sleep(3); break;
      case 4: __builtin_amdgcn_s_sleep(4); break;
      case 5: __builtin_amdgcn_s_sleep(6); break;
      case 6: __builtin_amdgcn_s_sleep(8); break;
      case 7: __builtin_amdgcn_s_sleep(12); break;
      case 8: __builtin_amdgcn_s_sleep(16); break;
      default: break;
    }
#endif
}

__global__ __launch_bounds__(totalWaves * 64) void
ringTest(unsigned totalStages, unsigned ks, unsigned *errors,
         double *checksums)
{
    __shared__ Ring ring;
    const int tid = threadIdx.x, lane = tid % 64, wave = tid / 64;
    if (tid < STAGES) {
        ring.full[tid] = 0;
        ring.empty[tid] = 0;
    }
    __syncthreads();
    if (wave >= consumerWaves) {
        const int pw = wave - consumerWaves;
        int slot = 0;
        unsigned fill = 0;
        for (unsigned q = 0; q < totalStages; ++q) {
            if (fill) {
                pipelineWait(&ring.empty[slot], fill * GROUP_WAVES);
            }
            jitter(q, wave, 1);
            for (int i = 0; i < perLaneProduce; ++i) {
                const int index = (pw * perLaneProduce + i) * 64 + lane;
                ring.stage[slot][index] = pattern(q, index);
            }
            pipelineSignal(&ring.full[slot], lane);
            if (++slot == STAGES) {
                slot = 0;
                ++fill;
            }
        }
        return;
    }
    const int g = wave / GROUP_WAVES;
    int slot = int((g * ks) % STAGES);
    unsigned fill = (g * ks) / STAGES;
    const unsigned skip = (GROUPS - 1) * ks;
    const unsigned skipFill = skip / STAGES, skipSlot = skip % STAGES;
    unsigned mismatches = 0;
    double checksum = 0;
    const unsigned tiles = totalStages / ks;
    for (unsigned i = g; i < tiles; i += GROUPS) {
        for (unsigned j = 0; j < ks; ++j) {
            const unsigned q = i * ks + j;
            if (!BROKEN) {
                pipelineWait(&ring.full[slot], (fill + 1) * PRODUCER_WAVES);
            }
            jitter(q, wave, 2);
            for (int e = 0; e < perLaneConsume; ++e) {
                const int index = e * 64 + lane;
                const float value = ring.stage[slot][index];
                mismatches += value != pattern(q, index);
                checksum += value;
            }
            jitter(q, wave, 3);
            pipelineSignal(&ring.empty[slot], lane);
            if (++slot == STAGES) {
                slot = 0;
                ++fill;
            }
        }
        fill += skipFill;
        slot += int(skipSlot);
        if (slot >= STAGES) {
            slot -= STAGES;
            ++fill;
        }
    }
    if (mismatches) {
        atomicAdd(&errors[blockIdx.x], mismatches);
    }
    checksums[(blockIdx.x * consumerWaves + wave) * 64 + lane] = checksum;
}

static bool
parse(std::string_view text, unsigned &value)
{
    auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.end() && value > 0;
}

int
main(int argc, char **argv)
{
    unsigned totalStages = 96, ks = 8, blocks = 8;
    for (int arg = 1; arg + 1 < argc; arg += 2) {
        std::string_view option(argv[arg]);
        unsigned *target = option == "--stages"   ? &totalStages
                           : option == "--ks"     ? &ks
                           : option == "--blocks" ? &blocks
                                                  : nullptr;
        if (!target || !parse(argv[arg + 1], *target)) {
            std::cerr << "usage: " << argv[0]
                      << " [--stages N] [--ks N] [--blocks N]\n";
            return 1;
        }
    }
    if (totalStages % ks) {
        std::cerr << "stages must be a multiple of ks\n";
        return 1;
    }
    std::cout << "ring_test producers=" << PRODUCER_WAVES
              << " groups=" << GROUPS << " group_waves=" << GROUP_WAVES
              << " stages=" << STAGES << " payload=" << PAYLOAD
              << " jitter=" << JITTER << " broken=" << BROKEN
              << " spin_sleep=" << SPIN_SLEEP << " total_stages="
              << totalStages << " ks=" << ks << " blocks=" << blocks << '\n';
    const size_t lanes = size_t(blocks) * consumerWaves * 64;
    std::vector<unsigned> errors(blocks, 0);
    std::vector<double> checksums(lanes, 0);
    unsigned *deviceErrors;
    double *deviceChecksums;
    if (hipMalloc(&deviceErrors, blocks * sizeof(unsigned)) != hipSuccess ||
        hipMalloc(&deviceChecksums, lanes * sizeof(double)) != hipSuccess) {
        std::cerr << "hipMalloc failed\n";
        return 1;
    }
    hipMemcpy(deviceErrors, errors.data(), blocks * sizeof(unsigned),
              hipMemcpyHostToDevice);
    hipMemcpy(deviceChecksums, checksums.data(), lanes * sizeof(double),
              hipMemcpyHostToDevice);
    hipLaunchKernelGGL(ringTest, dim3(blocks), dim3(totalWaves * 64), 0, 0,
                       totalStages, ks, deviceErrors, deviceChecksums);
    if (hipGetLastError() != hipSuccess ||
        hipDeviceSynchronize() != hipSuccess) {
        std::cerr << "kernel failed\n";
        return 1;
    }
    hipMemcpy(errors.data(), deviceErrors, blocks * sizeof(unsigned),
              hipMemcpyDeviceToHost);
    hipMemcpy(checksums.data(), deviceChecksums, lanes * sizeof(double),
              hipMemcpyDeviceToHost);
    hipFree(deviceErrors);
    hipFree(deviceChecksums);
    // Every consumer wave in group g reads exactly the stages of tiles
    // i = g, g + GROUPS, ...; each lane sums its PAYLOAD / 64 elements.
    unsigned failures = 0;
    for (unsigned block = 0; block < blocks; ++block) {
        failures += errors[block];
        for (int wave = 0; wave < consumerWaves; ++wave) {
            const int g = wave / GROUP_WAVES;
            for (int lane = 0; lane < 64; ++lane) {
                double expected = 0;
                for (unsigned i = g; i < totalStages / ks; i += GROUPS) {
                    for (unsigned j = 0; j < ks; ++j) {
                        const unsigned q = i * ks + j;
                        for (int e = 0; e < perLaneConsume; ++e) {
                            const int index = e * 64 + lane;
                            expected += double(
                                (q * 7u + unsigned(index) * 3u) % 251u);
                        }
                    }
                }
                const double got =
                    checksums[(block * consumerWaves + wave) * 64 + lane];
                if (got != expected) {
                    if (failures < 8) {
                        std::cerr << "checksum mismatch block " << block
                                  << " wave " << wave << " lane " << lane
                                  << " got " << got << " expected "
                                  << expected << '\n';
                    }
                    ++failures;
                }
            }
        }
    }
    if (failures) {
        std::cout << "ring_test: FAIL mismatches=" << failures << '\n';
        return 2;
    }
    std::cout << "ring_test: PASS total_stages=" << totalStages
              << " ring_reuses=" << totalStages / STAGES << " blocks="
              << blocks << '\n';
    return 0;
}
