#ifndef GPU_LAB_WG_PIPELINE_HH
#define GPU_LAB_WG_PIPELINE_HH

#include <hip/hip_runtime.h>

// Producer/consumer handshake between wavefronts of one workgroup, built on
// LDS counters. gfx942 S_BARRIER synchronises the whole workgroup, so waves
// with different roles cannot use it as a pairwise barrier, and DS_GWS_BARRIER
// is not implemented by the simulator. Each stage of an LDS ring therefore
// carries two monotonically increasing counters: "full" counts producer
// waves that have completed a fill, and "empty" counts consumer waves that
// have finished reading it. Waiting compares against a target computed from
// the fill number, so counters never need resetting.
//
// Ordering: a release fence (s_waitcnt lgkmcnt(0)) makes a wave's earlier LDS
// reads or writes complete before its counter increment, and an acquire fence
// after the observed value keeps later LDS accesses behind it. LDS traffic
// from one CU is serviced in issue order, so the fences also order the
// counter update against other waves' accesses. The counters are updated by
// one lane to avoid a 64-way bank conflict on one word.
#ifndef SPIN_SLEEP
// s_sleep multiples of 64 cycles between polls; 0 polls continuously. The
// simulator treats an s_sleep 0 as an unbounded sleep, so it is never used.
#define SPIN_SLEEP 1
#endif

__device__ __forceinline__ void
pipelineWait(unsigned *counter, unsigned target)
{
    while (static_cast<int>(
               __hip_atomic_load(counter, __ATOMIC_RELAXED,
                                 __HIP_MEMORY_SCOPE_WORKGROUP) -
               target) < 0) {
#if SPIN_SLEEP > 0
        __builtin_amdgcn_s_sleep(SPIN_SLEEP);
#endif
    }
    __builtin_amdgcn_fence(__ATOMIC_ACQUIRE, "workgroup");
}

__device__ __forceinline__ void
pipelineSignal(unsigned *counter, int lane)
{
    __builtin_amdgcn_fence(__ATOMIC_RELEASE, "workgroup");
    if (lane == 0) {
        __hip_atomic_fetch_add(counter, 1u, __ATOMIC_RELAXED,
                               __HIP_MEMORY_SCOPE_WORKGROUP);
    }
}

#endif
