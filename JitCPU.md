# RISC-V JitCPU proof of concept

## Status and scope

`RiscvJitCPU` is a single-hart, QEMU-TCG-backed CPU for fast functional
execution. It can boot RV64 Linux in `atomic_noncaching` mode and switch to a
`RiscvO3CPU` in timing mode. Reusing the same two CPU objects is supported in
both directions, including repeated sequences such as:

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

- RISC-V RV64, one hart, and one JitCPU backend instance per process.
- No vector or hypervisor extension support.
- Reverse switching to `atomic_noncaching` is implemented for CHI. Other Ruby
  protocols must implement the coherent-maintenance controller interface
  before they can claim equivalent support.
- JitCPU performs fast functional memory accesses; it does not model detailed
  cache or interconnect timing.
- Checkpoint restore, multicore/SMT, DMA races during maintenance, and
  performance targets beyond the current PoC need separate qualification.

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

## Bidirectional takeover

The gem5 `ThreadContext` remains the canonical switchable architectural state.
Normal `BaseCPU` takeover transfers PC, integer and floating-point registers,
privilege state, interrupts, CSRs, PMP, and MMU-visible state between the two
CPU objects. The existing RISC-V PMP takeover support is used in both
directions.

`RiscvJitCPU::takeOverFrom()` adds the QEMU-specific reverse-takeover step. On
every O3-to-JIT switch it:

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

The initramfs target creates a small static `/init`. It executes a userspace
`m5_exit` after Linux boot, then remains PID 1 and continuously updates a
256 KiB working set. This overlay is necessary for repeated switching because
the fixed image's original init script returns after its `m5_exit`, which
eventually causes Linux to panic after a one-way test has already ended.

## Directed bare-metal regression

`jitcpu-repeated-switch.S` prints `P00 J` through `P21 O` and uses `m5_fail`
with distinct codes for all failures. Its 21 switches provide ten complete
JIT/O3 round trips and a final O3 interval. Every phase checks instruction
progress and memory mode. JIT phases must produce exactly zero timing CHI
traffic; O3 phases must produce nonzero network messages, cache accesses, and
memory traffic.

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

Run the positive stress and its automated negative control:

```sh
python3 tests/gem5/jitcpu/run_repeated_switch_regression.py \
  build/RISCV/gem5.opt "$QEMU_JIT_LIBRARY" \
  --outdir /tmp/jitcpu-repeated-regression
```

The negative control deliberately bypasses Ruby maintenance for the first
O3-to-JIT switch. It is considered successful only when gem5 exits nonzero and
the payload reports `m5_fail instruction encountered (code 9)` from the stale
cached phase word. The same positive regression can exercise Garnet directly:

```sh
build/RISCV/gem5.opt -d /tmp/jitcpu-repeated-garnet \
  tests/gem5/jitcpu/configs/jitcpu_baremetal.py \
  tests/test-progs/jitcpu-smoke/src/jitcpu-repeated-switch \
  "$QEMU_JIT_LIBRARY" --ruby-chi --ruby-network garnet \
  --repeated-switches 21 --max-ticks 200000000
```

## Linux Ruby+CHI validation

The Linux test first boots the existing fixed RV64 image entirely on JitCPU
until the initramfs userspace `m5_exit`. Host-driven, bounded intervals then
reuse the same JitCPU and O3 objects. The test rejects a phase with no userspace
instruction progress, unexpected exit causes, the wrong memory mode, CHI
traffic during JIT, or missing CHI/cache/memory traffic during O3. It finishes
on O3, runs a longer final interval, and scans the terminal log for kernel
panic, Oops, BUG, access-fault, and unhandled-fault markers.

Run the complete bare-metal suite followed by an 11-switch Linux stress:

```sh
python3 tests/gem5/jitcpu/run_repeated_switch_regression.py \
  build/RISCV/gem5.opt "$QEMU_JIT_LIBRARY" \
  --linux-image /path/to/riscv-boot-exit-nodisk-1.0.0 \
  --linux-switches 11 \
  --outdir /tmp/jitcpu-full-regression
```

The equivalent direct Linux command is:

```sh
build/RISCV/gem5.opt -d /tmp/jitcpu-linux-repeated \
  tests/gem5/jitcpu/configs/jitcpu_linux.py \
  /path/to/riscv-boot-exit-nodisk-1.0.0 \
  "$QEMU_JIT_LIBRARY" --ruby-chi --repeated-switches 11 \
  --phase-ticks 100000000 --final-o3-ticks 1000000000 \
  --initrd tests/test-progs/jitcpu-smoke/src/jitcpu-linux-init.cpio \
  --max-ticks 2000000000000
```

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

- SimpleNetwork: 21 directed switches passed; every JIT interval reported
  zero CHI messages and every O3 interval reported nonzero network, cache, and
  memory activity.
- Garnet: 21 directed switches passed at tick 76,876,000 with the same traffic
  assertions. Drain diagnostics proved the post-maintenance DRAM write queue
  was empty before each transition to uncached mode.
- Negative control: skipping coherent maintenance failed at the first reverse
  handoff with `m5_fail` code 9, proving the positive result depends on the
  coherence fix.
- Linux: an 11-switch SimpleNetwork stress reached userspace on JitCPU,
  survived five complete JIT/O3 round trips, finished on O3, and completed a
  one-billion-tick final O3 interval. Every JIT phase had zero CHI traffic;
  every O3 phase had userspace progress and nonzero CHI/cache/DRAM activity;
  the guest log check passed.
- Existing one-way bare-metal and Linux switches passed with both classic
  memory and Ruby+CHI.

The directed test, not Linux alone, is the correctness gate: it explicitly
proves dirty writeback, clean invalidation, translation/TB invalidation, PMP,
interrupt, atomic, MMIO, and repeated state-transfer behavior before the Linux
stress is run.
