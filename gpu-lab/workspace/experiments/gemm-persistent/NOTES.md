# Persistent GEMM study: working notes

Model and platform findings made while preparing the study. Each item is
reproducible with the files named; the final report cites the resolved
numbers.

## Simulator fixes required for workgroups of eight or more waves

1. `src/gpu-compute/compute_unit.cc` compared a workgroup's total SGPR
   demand (waves x SGPRs per wave) against one SIMD's 800-register pool,
   while the neighbouring VGPR check multiplies by the number of SIMDs. LLVM
   raises the descriptor SGPR count to the architectural maximum (102, 104
   after granulation) whenever LDS usage limits occupancy, so any LDS kernel
   with eight or more waves per workgroup panicked at dispatch. The check now
   uses the CU total, matching the per-SIMD mapping performed afterwards.
2. `src/arch/amdgpu/vega/insts/ds.cc` returned early for DS instructions
   whose EXEC mask is empty without undoing the wave's local-memory
   reservation (`rdLmReqsInPipe`/`wrLmReqsInPipe`). The compiler emits such
   instructions for short `if (threadIdx.x < N)` blocks without an `execz`
   skip; the leaked count later panicked when the wave slot was reused.
   `Wavefront::releaseLmResource()` now balances the reservation.

Both are generic GPU-model corrections with no service knowledge. The LDS
unit tests (`lds_access.test.opt`, six tests) pass with the change.

## Global atomics on the gem5-kmd backend

`atomic-probe.cpp` has each workgroup's lane 0 add one to a per-workgroup
counter. On `gem5-kmd` every atomic is applied twice (counters read 2x the
repeat count and returned values follow the doubled sequence); the same
binary on `full-system` is correct. The GPU trace shows each atomic issued
once and the VIPER directory performing it once. The cause is Ruby's
`access_backing_store`, which the KMD launcher enables so the attached
process sees VRAM through the shared backing store: `RubyPort` re-executed
each completed atomic packet on physical memory after the directory had
already written the result through. Fixed in `GPUCoalescer` (commit the
protocol's result bytes, skip the re-execution); the probe passes on both
backends. The persistent kernels still count tiles in registers.

## Register allocator policy

The default `--reg-alloc-policy=simple` allocator admits a workgroup only to
SIMDs with no allocated waves and never checks capacity, so a four-wave
workgroup can be piled onto a single free SIMD, and with 129-VGPR waves the
bump allocator asserts (`SimplePoolManager::allocateRegion`) once three or
more workgroups share a CU. This crashed the double-buffered nonpersistent
control (32 workgroups) and every persistent variant with more than two
workgroups per CU. The audit already validated `--reg-alloc-policy=dynamic`,
which checks real capacity and spreads waves across SIMDs; all study
comparisons use it, passed as a configuration argument and recorded in each
run's `config_args`. Results measured under the simple allocator are kept
under the `px-` prefix for reference only.

## Synchronisation validation

`sync-test.cpp` drives the LDS ring protocol of `samples/common/
wg_pipeline.hh` with pattern-checked payloads, per-stage pseudo-random
`s_sleep` jitter, and one/two producer waves feeding one/two consumer groups
through rings of 2, 3, 4 and 6 slots (96 stages, 16-48 ring reuses, eight
workgroups). All variants pass on simdojo and gem5-kmd; `BROKEN=1` (no
consumer waits) fails with millions of mismatches on both, so the check has
teeth. gem5-kmd run ids are in `work/gemm-persist/sync/gem5-kmd-results.txt`.

## Kernel counts of the persistent samples

The persistent kernels write per-workgroup tile counts that the host copies
back; the runtime performs that copy with a blit kernel, so `gem5-kmd` runs
of the two optimized samples launch two modeled kernels (the verifier expects
two). An earlier version also zero-filled the buffer, adding a third; the
kernel now writes every slot exactly once and the fill was removed before the
final measurements. The blit's active time is a few hundred cycles.

## Sample defaults and control definitions

`hip-matmul-lds` now defaults to the specialised pipeline (`PRODUCERS=128`,
`GROUPS=2`, `STAGES=4`). Cooperative persistent controls must therefore pass
`PRODUCERS=0 GROUPS=1` explicitly; `finalize.py` and `explore.py` do so. One
finals variant labelled `coop-s2-w8` was built before that fix and is in fact
`spec-p128-g2-s2-f1-w8`; its directory and records were relabelled and the
cooperative finalists were remeasured.

## GPU TLB assertion of the register-only 64x64 persistent kernel

`tlb.cc:510` (`new_entry->pte` null) was a page fault in disguise: the walker
hit an invalid PDE, set a fault, but still handed the TLB an entry with a
zero PTE. The faulting address was always `a - 0x20000000`, produced by a
row index of 0xffe00000. Register-value tracing (`GPUVRF`) showed
`v_lshrrev_b16 v1, 1, v49` writing only the low 16 bits of `v1` and keeping
the upper 16 bits of the reciprocal temporary (0x0fffffe0) that an earlier
integer-division expansion had left there. gfx9-family VALU writes 16-bit
results zero-extended, which LLVM assumes when it follows the shift with a
32-bit `v_lshlrev_b32`. Fixed in `VecOperand::write()` (zero-extend
sub-dword results); the walker now reports page faults with the address.
