# RISC-V JitCPU proof of concept

## Status and scope

`RiscvJitCPU` is a QEMU-TCG-backed CPU for fast functional execution. It can
boot multicore RV64 Linux in `atomic_noncaching` mode and switch every hart to
a corresponding `RiscvO3CPU` in timing mode. Reusing the same CPU objects is
supported in both directions, including repeated sequences such as:

```text
JitCPU -> O3CPU -> JitCPU -> O3CPU -> ...
```

Ruby+CHI is supported across those switches. Entering JitCPU performs a
protocol-coherent hierarchy writeback and invalidation; drain alone is not
treated as cache invalidation.

The proof-of-concept ISA is RV64GC plus Zicsr and Zifencei, MSU privilege, and
Sv39. JitCPU and O3 use the same `RiscvISA` configuration. The current test
configurations disable V, H, Zicbom, and Zicboz so code accepted during a JIT
phase remains compatible with O3.

Current limitations are:

- RISC-V RV64 only. Four harts are qualified; larger systems have not yet been
  stress-tested.
- No vector or hypervisor extension support.
- One shared QEMU runtime owns a fixed number of vCPUs and one physical address
  space per gem5 process. All vCPUs execute through QEMU's single-threaded TCG
  mode, so multicore correctness is supported but host-side JIT execution is
  serialized and does not scale with the number of host cores.
- Reverse switching to `atomic_noncaching` is implemented for CHI. Other Ruby
  protocols must implement the coherent-maintenance controller interface
  before they can claim equivalent support.
- JitCPU performs fast functional memory accesses; it does not model detailed
  cache or interconnect timing.
- The Linux two-controller configuration uses Ruby's canonical functional
  backing store because interleaved DRAM ranges are not host-contiguous. The
  directed bare-metal test disables that shortcut and checks writeback and
  invalidation through both real interleaved controllers.
- Checkpoint restore, SMT, DMA races during maintenance, systems larger than
  four harts, and performance targets beyond the current PoC need separate
  qualification.

## Source and licensing layout

The pinned upstream QEMU source is the [`ext/qemu/repo`](ext/qemu/repo)
submodule. The gem5-owned adapter, smoke test, and minimal integration patch
live in [`ext/qemu/gem5-jit`](ext/qemu/gem5-jit). The build helper copies the
pinned tree and applies the patch without modifying the submodule checkout.
The parent repository's gitlink is the authoritative QEMU revision; builds do
not follow QEMU `master` or use an unrelated system QEMU.

The adapter and linked QEMU TCG/RISC-V code are GPL-2.0-or-later. Distributing
a gem5 build linked with or loading that combined backend must satisfy the GPL
requirements for the combined work, including corresponding source and
license notices. The normal gem5 binary does not link the adapter: it loads a
user-selected shared object at runtime, and a build with `USE_JITCPU=n` has no
QEMU dependency. This separation is useful engineering isolation, but it is
not a way to avoid applicable GPL obligations. Obtain legal review for any
distribution plan.

## Multicore backend model

All JitCPU objects load the same backend image. ABI version 5 creates one QEMU
RISC-V vCPU per gem5 JitCPU and selects it with an explicit backend instance
ID. Each instance has independent integer, floating-point, privilege, CSR,
interrupt, software-TLB, halted, exception, and LR/SC state. `mhartid` is the
gem5 context ID, rather than QEMU's creation-order default.

The embedded QEMU runtime and physical address space are shared. TCG executes
one vCPU at a time (`tcg,thread=single`), matching gem5's event-driven CPU
scheduling. Same-tick peer JitCPU events do not reduce a running hart to
one-instruction batches or force a hart in WFI to poll every simulated cycle.
External device/timer events at the same tick remain visible even when they
share an event-queue bin with JitCPU events. A backend batch also stops after
MMIO so device side effects cannot be repeated before gem5 services pending
events.

Machine interrupt-pending bits remain owned by gem5's interrupt controllers.
Only supervisor software/timer/external pending bits are imported from QEMU;
this preserves guest CSR/SBI updates without replaying a stale CLINT MSIP or
timer level after the device has cleared it.

## Bidirectional takeover

The gem5 `ThreadContext` remains the canonical switchable architectural state.
Normal `BaseCPU` takeover transfers PC, integer and floating-point registers,
privilege state, interrupts, CSRs, PMP, and MMU-visible state between the two
CPU objects. The existing RISC-V PMP takeover support is used in both
directions.

`RiscvJitCPU::takeOverFrom()` adds the QEMU-specific reverse-takeover step. On
every O3-to-JIT switch, for every hart, it:

1. completes `NonCachingSimpleCPU`/`BaseCPU` takeover;
2. synchronously flushes QEMU's software TLB;
3. queues and completes a full TCG translation-block flush;
4. clears stale halted/WFI, pending-exception, m5op, fault-address, and LR/SC
   reservation state; and
5. imports the new gem5 integer, FP, PC, privilege, interrupt, CSR, PMP, and
   SATP state into QEMU.

Flushing translations even when SATP is unchanged is required because O3 may
have edited page tables or executable memory during the preceding timing
phase.

## Ruby+CHI coherent maintenance

Switching from O3 timing mode to JitCPU `atomic_noncaching` mode follows this
ordering:

1. `m5.switchCpus(..., is_ruby=True)` drains the CPUs and outstanding Ruby
   work and switches O3 out.
2. The CHI HNF, as point of coherence, walks its directory and local cache one
   address at a time.
3. Each address uses normal CHI replacement machinery. Upstream RNF copies are
   invalidated with `SnpCleanInvalid`; dirty data is collected and written to
   the downstream memory node, while clean lines are invalidated without an
   unnecessary data write.
4. Maintenance finishes only when every participating cache and directory is
   empty, all CHI TBE/internal queues are empty, and all Ruby networks are
   quiescent. SimpleNetwork checks endpoint, internal-link, and switch queues.
   Garnet additionally tracks non-statistical in-flight packets and waits for
   data and credit-link source/destination buffers, so statistics resets
   cannot falsify quiescence.
5. The drain manager is re-armed and the system is drained again. This final
   drain waits for posted writebacks already accepted by CHI but still queued
   in the DRAM controller.
6. Only then does gem5 change to `atomic_noncaching` and invoke JitCPU reverse
   takeover.

`RubySystem::memInvalidate()` rejects an attempt to continue while coherent
maintenance is incomplete. The CHI implementation never clears tags without
also completing the corresponding protocol and directory transitions.

## Build

Install the host packages listed in the QEMU adapter README, including a C/C++
toolchain, Meson/Ninja dependencies, SCons, Python development headers, the
RISC-V GNU cross compiler, `device-tree-compiler`, and `cpio`. Initialize and
build the pinned adapter:

```sh
git submodule update --init ext/qemu/repo
util/jitcpu/build-qemu-jit.sh /tmp/qemu-jit-build
export QEMU_JIT_LIBRARY=/tmp/qemu-jit-build/libgem5-qemu-jit.so
```

Configure an actual CHI-enabled gem5 binary and build the test payloads:

```sh
scons setconfig build/RISCV \
  USE_JITCPU=y PROTOCOL=CHI RUBY_PROTOCOL_CHI=y
scons build/RISCV/gem5.opt -j"$(nproc)"
make -C tests/test-progs/jitcpu-smoke/src
```

The initramfs targets create small static `/init` payloads. The single-core
version executes a userspace `m5_exit` after Linux boot and then continuously
updates memory. The SMP version starts and pins four workers before its first
`m5_exit`; together they continuously update a 256 KiB working set and a
shared atomic counter. These overlays are necessary for repeated switching
because the fixed image's original init script returns after its `m5_exit`,
which eventually causes Linux to panic after a one-way test has already ended.

## Directed bare-metal regression

The single-hart `jitcpu-repeated-switch.S` payload prints `P00 J` through
`P21 O` and uses `m5_fail` with distinct codes for all failures. Its 21
switches provide ten complete JIT/O3 round trips and a final O3 interval.
Every phase checks instruction progress and memory mode. JIT phases must
produce exactly zero timing CHI traffic; O3 phases must produce nonzero
network messages, cache accesses, and memory traffic.

The payload covers:

- PC/continuation, integer and floating-point registers, privilege mode,
  SSCRATCH, SATP, and an active PMP rule while executing in S-mode;
- 64 dirty lines distributed at 4 KiB strides, written by O3 and observed by
  JIT after writeback;
- 64 clean O3-resident lines overwritten behind the hierarchy by JIT and then
  observed by O3 after invalidation;
- page-table edits in both directions with unchanged SATP and `SFENCE.VMA`;
- self-modifying code in both directions with `FENCE.I`;
- checked AMO and LR/SC results in both CPU models;
- UART MMIO, CLINT timer interrupts, and repeated m5ops; and
- phase-word persistence, which makes a skipped writeback immediately visible.

The four-hart `jitcpu-smp-repeated-switch.S` payload adds a 64 KiB private
footprint per hart, distinct preserved integer and floating-point values per
hart, shared AMOs, and a generation barrier around every handoff. It fails if
any hart skips or duplicates a phase. The host checker requires progress and
cache activity from every hart/RNF and memory traffic through each of two
interleaved controllers. Ruby's canonical backing store is disabled in this
test, so dirty writeback and clean invalidation are checked against the real
controller backing stores.

The complementary `jitcpu-smp-wfi-repeated-switch.S` payload keeps hart 0
active while harts 1-3 repeatedly enter WFI. After every whole-system
handoff, hart 0 publishes a new generation and wakes each peer through its
own CLINT MSIP register. The peers handle and clear the machine software
interrupt, validate preserved integer/FP state and the preceding memory
footprint, update their next footprint, and return to WFI. Spurious WFI
returns permitted by the ISA are handled without weakening skipped-generation
checks. This covers active-to-suspended and suspended-to-active takeover,
per-hart interrupt routing, and interrupt clear/re-arm across both CPU models.
An independent per-hart trap counter requires exactly one machine software
interrupt to be handled in every phase, so a legal spurious WFI return cannot
produce a false pass.

Run both directed stresses, the automated negative control, and the 4-core
SimpleNetwork and Garnet variants:

```sh
python3 tests/gem5/jitcpu/run_repeated_switch_regression.py \
  build/RISCV/gem5.opt "$QEMU_JIT_LIBRARY" \
  --smp-garnet \
  --outdir /tmp/jitcpu-repeated-regression
```

The negative control deliberately bypasses Ruby maintenance for the first
O3-to-JIT switch. It is considered successful only when gem5 exits nonzero and
the payload reports `m5_fail instruction encountered (code 9)` from the stale
cached phase word. The multicore Garnet regression can also be run directly:

```sh
build/RISCV/gem5.opt -d /tmp/jitcpu-repeated-garnet \
  tests/gem5/jitcpu/configs/jitcpu_baremetal.py \
  tests/test-progs/jitcpu-smoke/src/jitcpu-smp-repeated-switch \
  "$QEMU_JIT_LIBRARY" --ruby-chi --ruby-network garnet \
  --num-cpus 4 --num-dirs 2 --num-l3caches 4 \
  --repeated-switches 21 --max-ticks 200000000
```

## Linux Ruby+CHI validation

The Linux test first boots the existing fixed RV64 image entirely on JitCPU
until the initramfs userspace `m5_exit`. The SMP initramfs creates four
shared-address-space workers, pins one to each Linux CPU, verifies the actual
CPU assignment, and requires every worker to sweep private memory and perform
shared AMOs before signaling the host. The workers then remain active for all
switch phases.

Host-driven, bounded intervals reuse the same JitCPU and O3 objects. The test
rejects a phase with no userspace instruction progress on any core, unexpected
exit causes, the wrong memory mode, CHI traffic during JIT, or missing CHI
traffic from any RNF or memory controller during O3. It finishes on O3, runs a
one-billion-tick final interval, and scans the terminal log for kernel panic,
Oops, BUG, access-fault, and unhandled-fault markers.

Run the complete suite: 11 switches on single-core Linux, plus 21 switches on
4-core/4-HNF/2-controller Linux with both SimpleNetwork and Garnet:

```sh
python3 tests/gem5/jitcpu/run_repeated_switch_regression.py \
  build/RISCV/gem5.opt "$QEMU_JIT_LIBRARY" \
  --linux-image /path/to/riscv-boot-exit-nodisk-1.0.0 \
  --linux-switches 11 \
  --smp-linux-switches 21 --smp-garnet \
  --outdir /tmp/jitcpu-full-regression
```

The equivalent direct multicore SimpleNetwork command is:

```sh
build/RISCV/gem5.opt -d /tmp/jitcpu-linux-repeated \
  tests/gem5/jitcpu/configs/jitcpu_linux.py \
  /path/to/riscv-boot-exit-nodisk-1.0.0 \
  "$QEMU_JIT_LIBRARY" --ruby-chi \
  --num-cpus 4 --num-dirs 2 --num-l3caches 4 \
  --repeated-switches 21 \
  --phase-ticks 100000000 --final-o3-ticks 1000000000 \
  --initrd tests/test-progs/jitcpu-smoke/src/jitcpu-linux-smp-init.cpio \
  --max-ticks 2000000000000
```

Add `--ruby-network garnet` to repeat the same Linux stress on Garnet.

## One-way compatibility checks

The original one-way JIT-to-O3 paths remain available. Bare-metal classic and
CHI examples are:

```sh
build/RISCV/gem5.opt -d /tmp/jitcpu-switch-classic \
  tests/gem5/jitcpu/configs/jitcpu_baremetal.py \
  tests/test-progs/jitcpu-smoke/src/jitcpu-switch \
  "$QEMU_JIT_LIBRARY" --switch-to-o3 --max-ticks 10000000

build/RISCV/gem5.opt -d /tmp/jitcpu-switch-chi \
  tests/gem5/jitcpu/configs/jitcpu_baremetal.py \
  tests/test-progs/jitcpu-smoke/src/jitcpu-switch \
  "$QEMU_JIT_LIBRARY" --ruby-chi --switch-to-o3 \
  --max-ticks 10000000
```

For build isolation, set `USE_JITCPU=n` and rebuild. That binary must neither
export `RiscvJitCPU` nor depend on the QEMU adapter. Restore the CHI/JIT
configuration before running this document's tests.

## Observed validation

Validation on 2026-07-22 used the pinned `ext/qemu/repo` revision and a
CHI-enabled `build/RISCV/gem5.opt`:

- The standalone ABI-5 adapter smoke created two QEMU vCPUs with different
  `mhartid` values and proved independent PC and integer-register state.
- The original single-hart directed regression passed 21 switches at tick
  66,266,000, preserving the original switch path after the multicore changes.
- Four-hart directed SimpleNetwork and Garnet regressions each passed 21
  switches, at ticks 828,691,000 and 936,397,000 respectively. Every phase
  advanced all four harts. Every JIT phase had zero timing CHI messages; every
  O3 phase exercised all four RNFs and both interleaved memory controllers.
  Each payload phase also checked per-hart integer/FP state, the complete
  64 KiB-per-hart footprint, shared AMO totals, and handoff generation.
- Four-hart WFI/MSIP SimpleNetwork and Garnet regressions passed 21 switches
  at ticks 1,801,647,000 and 1,910,067,000. Three harts repeatedly waited in
  WFI and were independently awakened, handled, and cleared by CLINT software
  interrupts after every switch. The trap counters reached the exact expected
  generation on every hart; all per-hart, RNF, and controller assertions
  passed.
- Negative control: skipping coherent maintenance failed at the first reverse
  handoff with `m5_fail` code 9, proving the positive result depends on the
  coherence fix.
- The single-hart Linux compatibility stress passed 11 switches and a
  one-billion-tick final O3 interval with a clean guest log.
- Four-hart Linux booted entirely on JitCPU and passed 21 switches with both
  SimpleNetwork and Garnet, finishing at ticks 627,206,071,000 and
  627,305,089,000. All four pinned userspace workers progressed in every
  phase. Each JIT phase carried zero timing CHI traffic. Every O3 phase had
  nonzero traffic on all four RNFs and both controllers, including the
  one-billion-tick final phase; guest-log scans passed on both networks.
- The automated bare-metal driver, including the negative, active-hart, and
  WFI/MSIP cases on both networks, passed end to end. Its single-core and SMP
  Linux branches were also exercised; the final per-node Linux checks passed
  in the direct 21-switch runs above.
- Existing one-way bare-metal and Linux switches passed with both classic
  memory and Ruby+CHI.

The directed test, not Linux alone, is the correctness gate: it explicitly
proves dirty writeback, clean invalidation, translation/TB invalidation, PMP,
interrupt, atomic, MMIO, and repeated state-transfer behavior before the Linux
stress is run.
