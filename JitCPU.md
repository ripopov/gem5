# RISC-V JitCPU proof-of-concept plan

## Goal

Add a single-hart `RiscvJitCPU` that uses dynamic binary translation to boot
64-bit RISC-V Linux faster than `RiscvNonCachingSimpleCPU`, then switches at an
`m5_switch_cpu` boundary to `RiscvO3CPU` for detailed simulation.

The PoC ISA is **RV64GC + Zicsr + Zifencei, MSU privilege, and Sv39**. The JIT
and O3 cores must share the same `RiscvISA` configuration. Disable V, H,
Zicbom, Zicboz, and optional bit-manipulation extensions until both execution
paths pass the same tests. This subset is intentionally narrower than O3CPU's
capabilities, so every instruction emitted or advertised by JitCPU remains
executable after the switch.

## Success criteria

- Boot the constrained RV64 Linux image to userspace with one hart.
- Execute `m5_switch_cpu`, transfer complete architectural state, and continue
  correctly on `RiscvO3CPU`.
- Run and validate a benchmark after the switch, with statistics collected only
  for the O3 region of interest.
- Match `AtomicSimpleCPU` architectural state at translation-block boundaries
  in differential tests.
- Achieve a measurable CPU-bound speedup over `NonCachingSimpleCPU` (target at
  least 2x for the PoC benchmark).
- After classic-memory validation, support starting with Ruby+CHI in
  `atomic_noncaching` mode and using normal CHI timing requests after switching
  to O3.

## Architecture

Model the control path after `BaseKvmCPU`: execute translated blocks up to an
instruction/event budget, return to gem5 at safe boundaries, and keep a
`SimpleThread`/`ThreadContext` as the canonical switchable architectural state.

The translation backend exposes a small C interface:

```text
create/destroy vCPU
map RAM and register MMIO callbacks
run(max_instructions) -> exit reason
get/set registers, PC, privilege state, and CSRs
inject interrupt and invalidate translations
```

Use `atomic_noncaching` memory mode. Fast-path ordinary RAM where correctness
allows it; exit to gem5 helpers for MMIO, faults, atomics/LR-SC, page-table
walks, and pseudo-instructions. Translation blocks are keyed by PC, privilege,
address-space/translation state, and relevant ISA flags. Invalidate them on
`fence.i`, `sfence.vma`, `satp` changes, code modification, drain, and restore.

Keep the QEMU-derived backend optional at build time. Before importing code,
pin the QEMU revision and record a file-by-file license audit. A build linked
with GPL-covered QEMU RISC-V code must be treated and distributed as a
GPL-compliant build; the normal non-JIT gem5 build must remain unaffected.

### QEMU source

The upstream QEMU source is pinned as the [`ext/qemu`](ext/qemu) submodule.
The parent repository's gitlink is the authoritative revision; builds must not
silently follow QEMU `master` or use an unrelated system installation. Limit
the integration to the RISC-V translator, TCG runtime/backends, and the minimum
support code required by the C adapter. Record every imported file and its
license. Commit required QEMU-side changes in a maintained fork, then update
the parent gitlink deliberately.

## Implementation stages

1. **Baseline and skeleton**
   - Record Linux-boot and CPU-bound `hostInstRate` for Atomic and NonCaching.
   - Add `BaseJitCPU`, `RiscvJitCPU`, build configuration, `CPUTypes.JIT`, and
     the standard-library factory mapping.
   - Implement CPU lifecycle, ports, instruction accounting, event budgeting,
     interrupts, drain, serialization hooks, and `atomic_noncaching` mode.

2. **RV64 translation backend**
   - Integrate the pinned backend behind the narrow C API.
   - Implement RV64GC integer/FP state, block lookup/chaining, precise side
     exits, and interpreter/helper fallback for unsupported operations.
   - Add a software TLB/direct-RAM fast path only after the helper-based path is
     correct.

3. **Full-system execution**
   - Implement MSU privilege state, Sv39 translation, traps, CSRs, CLINT/PLIC
     interrupts, `WFI`, fences, LR/SC and AMOs.
   - Recognize gem5 RISC-V m5ops, especially `m5_switch_cpu`, stats reset/dump,
     and exit.
   - Boot the constrained Linux image under classic memory.

4. **JitCPU to O3CPU switching**
   - On drain, stop at an instruction boundary, commit memory effects, and copy
     every live register/CSR/PC field into `ThreadContext`.
   - Set `support_take_over()` and validate the existing `m5.switchCpus()` path
     from `atomic_noncaching` to `timing` mode.
   - Compare JIT and Atomic state immediately before takeover, then run the
     benchmark warm-up and measured ROI on O3. One-way JIT-to-O3 switching is
     sufficient for the PoC.

5. **Ruby+CHI compatibility**
   - Instantiate CHI from startup; bypass it during JIT execution through
     Ruby's supported atomic-noncaching path, then use it normally under O3.
   - Configure the shared L1 controllers with `send_evictions=True`, as required
     by the future RISC-V O3 core even though JitCPU starts first.
   - Initially route JIT data accesses through Ruby atomic ports. Do not enable
     a direct backing-store path until DMA/coherence correctness is proven.
   - Verify that O3 begins with cold caches and produces expected CHI/cache/DRAM
     traffic after the switch.

## Testing strategy

Testing proceeds in gates; a later gate is enabled only after the earlier one
is stable.

1. **Build and isolation:** build gem5 with JIT disabled and enabled, verify the
   non-JIT build has no QEMU dependency, and check that the configured QEMU
   revision matches the `ext/qemu` gitlink.
2. **Directed execution:** run small bare-metal tests for each supported opcode
   class plus block exits, register synchronization, invalidation, MMIO,
   faults, interrupts, instruction limits, LR/SC, AMOs, and fences. Unsupported
   instructions must take a controlled fallback or illegal-instruction exit.
3. **Differential testing:** execute deterministic and generated RV64 programs
   under JitCPU and AtomicSimpleCPU. At every translation-block exit compare
   integer/FP registers, PC, privilege state, relevant CSRs, exception state,
   instruction count, and modified memory. AtomicSimpleCPU is the PoC oracle.
4. **Full-system testing:** boot one fixed, RV64GC-constrained Linux image to a
   userspace pass marker; cover timer/external interrupts, page faults, system
   calls, `WFI`, and all required m5ops. Use deterministic inputs and bounded
   timeouts so failures are reproducible.
5. **Switch testing:** boot with JitCPU, execute `m5_switch_cpu`, compare state
   immediately before takeover, and complete benchmark warm-up and ROI under
   O3. Check the result marker, exit cause, statistics reset boundary, memory
   mode, and a checkpoint smoke test.
6. **Ruby+CHI testing:** repeat the switch test with CHI present from startup.
   Confirm coherent memory contents, cold O3 caches at takeover, no unintended
   timing-cache traffic during JIT execution, and nonzero CHI/cache/DRAM traffic
   during the O3 ROI.
7. **Performance testing:** use optimized builds with tracing disabled, run at
   least three repetitions, and report the median wall time, `hostInstRate`,
   translation-cache hit rate, generated-code size, and exit-reason counts.
   Gate the PoC on the agreed CPU-bound speedup over NonCachingSimpleCPU while
   keeping correctness tests outside the timed region.

Keep short directed and differential tests in presubmit CI. Run Linux boot,
Ruby+CHI, and performance suites as scheduled or explicit extended tests.

## Deferred work

RV32, vector and hypervisor extensions, multicore/SMT, reverse O3-to-JIT
switching, detailed cache timing during JIT execution, and a generally stable
embedding API are outside the PoC.
