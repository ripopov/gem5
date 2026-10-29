# Goal: correct LDS timing and an evidence-based GEMM teaching workload

Work autonomously following AGENTS.md. Read `gpu-lab/EightCuEvaluation.md`.
Use local tools/images; preserve unrelated changes.

## 1. Minimal, upstreamable LDS fix

Match documented gfx942 behavior:
- https://rocm.docs.amd.com/projects/composable_kernel/en/7.13.0-preview/conceptual/ck_tile/hardware/lds_bank_conflicts.html
- https://rocm.docs.amd.com/projects/composable_kernel/en/7.13.0-preview/conceptual/ck_tile/hardware/gemm_optimization.html#ck-tile-gemm-optimization

Verify and fix the essential defects: 4-byte bank addressing; effective
addresses including offsets and multiword/dual-address accesses; active-lane
masks and read broadcasts; documented instruction-specific B128 access phases.
Separate baseline service cost from extra bank-conflict serialization in timing
and statistics. Preserve atomic semantics and avoid double-counting latency.
Reuse existing pipelines/queues. No service-specific behavior, workload special
cases, or artificial penalties to make LDS win.

Add focused tests for conflict-free and strided accesses, broadcasts/masks,
offsets, wide instructions, and documented padding/XOR layouts. Check both
functional correctness and timing/conflict accounting. Make exactly ONE clean
commit containing the generic LDS fix and its tests/supporting documentation,
with a valid gem5 tag, normal hooks, and a clear rationale and test summary.
Keep lab work separate; avoid unnecessary abstractions.

## 2. Optimize GEMM

Keep the default eight-CU hardware fixed across comparisons. Starting with
M=128, N=256, K=128, explore output shapes, K depths, and workgroup counts;
include K=256 and K=512. Do not require eight K iterations or one workgroup per CU.

Implement and tune both samples:
- `gpu-lab/workspace/samples/hip-matmul-lds/main.cpp`: LDS and register tiling,
  double buffering, and software pipelining that overlaps future global loads
  with current-tile computation. Evaluate wavefront specialization inside a
  workgroup against cooperative loading; retain and explain the best variant.
- `gpu-lab/workspace/samples/hip-matmul-regtiles/main.cpp`: maximize performance
  using registers and cache reuse, with no LDS use. Verify zero LDS allocation
  and no LDS accesses in generated code.

Compare tile sizes, threads/waves per workgroup, register pressure, unrolling,
prefetching, and LDS layouts. Inspect generated ISA to establish actual overlap,
wait/barrier placement, resource use, and absence of spills where claimed.
Alternating two buffers alone is not evidence of a pipeline. Use identical
FP32 arithmetic contracts, inputs, dimensions, and correctness checks for both
optimized kernels and the naive baseline. Never weaken one competitor.

Select a practical final shape for the naive sample that supports the
teaching progression without changing matrix sizes between implementations.
Aim for roughly one minute or less of naive simulation wall time on this host;
report any necessary tradeoff. Seek measured double-buffering benefits over a
tuned single-buffer LDS control. If cache-only GEMM wins, explain why with evidence; never force an LDS win.

## 3. Evidence and delivery

Measure correctness, wall time excluding builds, and GPU-active simulated ticks
and cycles using the configured clock. Keep launch, compiler, and cache-state
conditions comparable; repeat finalists to assess variation. Record commands,
run IDs, tuning results, traffic, stalls, conflicts, resources, and overlap.
Do not assume eight CUs cannot saturate memory bandwidth. Distinguish measured
behavior from model limitations and conclusions requiring real hardware.

Integrate the samples, update affected lab/architecture documentation, and run
all applicable AGENTS.md checks and backend gates. Commit lab changes separately.
Deliver the selected shape, reproducible results, ablation comparisons,
remaining limitations, and commit IDs. Report blocked checks honestly.
