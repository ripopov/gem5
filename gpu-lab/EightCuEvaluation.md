# Eight-CU teaching configuration: evaluation and limits

> Historical measurements: the [MI300X configuration audit](MI300XConfiguration.md)
> changes register capacity, wave slots, caches and clocks. Results below retain
> their original provenance and are not timings of the revised defaults.

The eight-CU configuration is a useful, faster default for the tested lab
workloads. It preserves the instruction, register, cache, LDS, queue, and
synchronization mechanisms used by those workloads. **Memory pressure is not
its only limitation, and eight CUs do not guarantee freedom from HBM bandwidth
bottlenecks.** This report preserves the measurements from the original
eight-CU evaluation. The subsequent [GEMM study](GemmOptimization.md) records
the generic LDS correction, its validation and the new kernel comparisons.

## What changed

The naive sample now computes `A[128][128] × B[128][256] → C[128][256]`, using
128 workgroups of 256 threads. The checksum is `51380224`. An optimized 64×64
output tile produces eight workgroups, each traversing eight K tiles of depth
16. The naive sample remains separate from the evaluation's optimized probes.

Both gem5 lab entry points default to eight CUs; explicit CU overrides remain
available. The tested model has four SIMDs and ten wave slots per SIMD, wave64,
64 KiB LDS per CU, 16 KiB private vector L1 per CU, and 256 KiB shared L2. The
single default HBM interface is 128 bits wide with a 64-byte burst every 4 ns,
for a theoretical 16 GB/s peak. This is a small teaching GPU, not an MI300X XCD.

The no-guest configuration now derives its ROCr topology from the configured
CU/SIMD/wave counts, exposes one logical XCC, and reports the configured VRAM
and LDS capacities. Previously it advertised 320 CUs and eight XCCs independently
of the number gem5 modeled. The corrected HIP query reports eight CUs, wave64,
64 KiB shared memory per block, and 1 GiB global memory in the default runner.

## Measured GEMM results

Measurements were made locally on 2026-09-15 with `gfx942`, `hipcc -O2`, and the
`gem5-kmd` backend at 1 GHz. GPU-active duration is `shaderActiveTicks / simFreq`;
cycles are `shaderActiveTicks / 1000`. Each GEMM run launched one GPU kernel.

| Kernel | GPU-active cycles, rounded | GPU-active time | Speedup over new naive |
| --- | ---: | ---: | ---: |
| Naive, one output per thread | 372,488 | 0.372488 ms | 1.00× |
| 16×16 cooperative LDS tiling | 204,707 | 0.204707 ms | 1.82× |
| 64×64 output tile, 4×4 outputs per thread | 137,399 | 0.137399 ms | 2.71× |

Every result was compared element by element with a CPU reference. The
register-tiled kernel completed four wavefronts on each of the eight CUs,
confirming that all eight CUs received a workgroup in this run. These probes
are examples for evaluation, not tuned performance limits.

The isolated new naive run took **48.83 s launcher wall time**, excluding
compilation, and **40.44 s gem5 host time**. The previous 40-CU, larger-matrix
run took 422.47 s and 400.61 s respectively. This is a before/after lab-runtime
comparison, not a controlled CU-count scaling experiment: the matrix size,
cache working set, and reported runtime topology all changed. Other probe
jobs ran concurrently; their host times are not comparable benchmarks.

## Memory pressure

A and B now total **192 KiB**, versus **416 KiB** in the larger example. The
smaller input working set fits in the 256 KiB shared L2 by capacity, though
output writes, conflicts, and other traffic can still evict it. Reducing
repeated global loads can therefore improve the CU's memory pipeline without
producing a proportional reduction in HBM traffic. Private cache and register
capacity per CU are unchanged; aggregate private cache capacity decreases.

The separate streaming probe reads **32 MiB** of `float4` input once with 128
workgroups. It verifies every thread's sum. On the default eight-CU model it
achieved **6.18 GB/s useful input throughput**, with **5.429065 ms GPU-active
time**. That is below the 16 GB/s HBM peak, but it is not proof that every
possible workload is free of memory-bandwidth limits. Latency, request
concurrency, access pattern, and the shared interconnect also constrain traffic.

The broader memory variants produced these rates:

| Configuration | GPU-active time | Useful input throughput |
| --- | ---: | ---: |
| Default 16 GB/s interface and fabric | 5.429065 ms | 6.18 GB/s |
| 8 GB/s interface, default fabric | 10.963589 ms | 3.06 GB/s |
| Default interface, `--bw-scalor=1` | 5.497587 ms | 6.10 GB/s |
| 8 GB/s interface, `--bw-scalor=1` | 11.162955 ms | 3.01 GB/s |

These runs show memory-system sensitivity even with eight CUs. Widening the
fabric did not improve this probe. The 8 GB/s interface also changes bank count,
row size, and refresh timings; `--bw-scalor=1` also changes L2 to 1 MiB with
different banking. Those comparisons alone cannot isolate peak bandwidth as
the sole cause of the slowdown.

A controlled follow-up changed **only `tBURST` from 4 ns to 2 ns**, including
its automatically derived minimum/maximum values. All other DRAM parameters
were identical in `config.ini`; CU count, caches, fabric, input, and executable
were unchanged. It passed with **3.636795 ms GPU-active time and 9.23 GB/s useful
throughput**, a **49% throughput improvement** over the default 6.18 GB/s.
The configured theoretical peak became 32 GB/s. This is a synthetic timing
sensitivity test, not a physical HBM profile or a proposed default.

Thus memory service bandwidth materially limits this streaming workload even
with eight CUs. A bandwidth limitation does not require observing the
interface's theoretical peak: scheduling, latency, and other parts of the
memory path leave some cycles unused. The statement “eight CUs can never be
bottlenecked by HBM bandwidth” is contradicted by this experiment.

Use useful input bytes divided by GPU-active time for this probe's throughput.
The DRAM counters include initialization and copies outside the kernel; their
whole-run utilization must not be interpreted as kernel-only utilization.

## Limits that matter for study

### Occupancy and scale

Eight output workgroups of 256 threads offer just four wavefronts per CU when
each CU receives one group, against 40 available wave slots. This limits
experiments about hiding latency with multiple resident workgroups. The same
limitation applied to the proposed 40-CU/40-workgroup layout. Use smaller output
tiles or a larger grid for an occupancy study, keeping input dimensions fixed
within each baseline/optimized comparison. A rectangular matrix also makes
row/column access experiments sensitive to the distinct strides M, N, and K.

Eight CUs cannot represent contention or load balancing across a full XCD or
multiple XCCs. The L2 and memory system have not been proportionally scaled
with CU count. `--bw-scalor` is not a pure bandwidth knob: it changes shared
L2 capacity and banking as well as network bandwidth.

### LDS bank mapping: defect found in the original evaluation

The stride-1 and stride-32 32-bit LDS probes both produced correct outputs.
Their CU0 mean `ldsBankConflictDist` values are **7** and **61.30** respectively;
whole-process GPU-active durations are 21.186 and 47.849 microseconds. Each
process also launched a ROCr copy kernel, so those durations are not isolated
LDS-kernel latencies.

The nonzero stride-1 value exposed a substantive model discrepancy: the
LDS model selected the bank with `byte_address % banks`.
For the basic 32-bit case, AMD documents
`(byte_address / 4) % 32` for 32 four-byte banks. Consequently, contiguous
32-bit accesses used only eight of its 32 bank indices. The model also
aggregated phases and used a simple conflict penalty. See
[AMD's bank-mapping documentation](https://rocm.docs.amd.com/projects/composable_kernel/en/7.13.0-preview/conceptual/ck_tile/hardware/lds_bank_conflicts.html).

This was a generic GPU-model issue, present with 40 CUs as well. The subsequent
fix counts word footprints, active lanes, broadcasts and access phases. The
old measurements above remain evidence of the defect, not predictions from
the corrected model. See the [new study's validation and limits](GemmOptimization.md#validation-and-limits-found-so-far)
before interpreting padding or swizzle experiments.

### Runtime-visible topology

The corrected `gem5-kmd` configuration reports eight CUs to HIP and ROCr.
The packaged full-system guest still reports **320 CUs**, while its
`config.ini` confirms eight modeled CUs. The register-tiled probe passed in
that guest, but launch sizing, occupancy calculations, and library heuristics
based on HIP's device properties can be misleading. Fixing that requires
aligning the guest-visible device description with the model, rather than
changing the application's device query or bypassing GPU behavior.

Simdojo retains its own functional topology (320 CUs in the supplied config).
Its passes validate computation, not eight-CU timing or occupancy.

### Service memory configuration

The default single-interface configuration passed. An attempted
`--dgpu-num-dirs=2` run failed before listener publication:
`device memory system.gpu_mem_ctrls0.dram has no writeable setup backdoor`.
The service's setup-memory path currently cannot obtain the writable backdoor
for this interleaved memory configuration. Multiple GPU memory interfaces are
therefore not a supported no-guest experiment yet. This predates the CU change
and must be addressed in the service/platform, not by bypassing GPU memory.

The other existing service limits remain: one attached process, one GPU,
no external SVM migration, queue suspension, cross-process IPC workload, or
connected checkpoint. These are distinct from CU count.

## Validation scope and reproduction

All four sample applications passed on **simdojo, gem5-kmd, and full-system**.
The tiled, register-tiled, and LDS probes passed on simdojo and gem5-kmd;
register-tiled GEMM also passed in full-system. Completed no-guest probe runs
were checked for successful application/simulator exit, AQL retirement, and
drained KMD state. Full-system model configurations were checked for eight
actual ComputeUnit objects.

This validates the exercised study mechanisms and workloads. It does not
establish ISA-wide correctness, calibrated physical-MI300X timing, exhaustive
resource-exhaustion behavior, or AArch64 runtime support for the changed
configuration; the evaluation host and images were x86-64.

Build/run commands and a result-checking summarizer are in
[the evaluation probes](workspace/experiments/eight-cu/README.md). General
backend commands and the naive wall-time procedure are in
[Running the lab](RunningTheLab.md).

Primary run IDs under `workspace/work/runs/`:

- `hip-matmul-gem5-kmd-20260915T070617Z-274602-22207`
- `8cu-tiled16-gem5-kmd-20260915T070915Z-283069-3664`
- `8cu-register64-gem5-kmd-20260915T070939Z-287152-4776`
- `8cu-stream-gem5-kmd-20260915T070916Z-283865-14649`
- `8cu-stream-burst2ns-gem5-kmd-20260915T072143Z-326113-6889`
- `8cu-register64-topology-full-system-20260915T071034Z-294709-18172`

The local runner image was updated offline by layering the changed launcher and
Python configuration onto the existing image; its compiled simulator and KMD
library were unchanged. The ordinary runner Dockerfile packages those same
source files for a clean build. The previous image remains available locally
as `gem5-gpu-hsakmt-runner:before-8cu`.
