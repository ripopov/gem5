# RISC-V JitCPU architecture and integration specification

## 1. Status

`RiscvJitCPU` is a fast functional gem5 CPU backed by QEMU's RISC-V TCG
translator. It is intended to accelerate uninteresting execution, such as
full-system boot, before switching the same harts to `RiscvO3CPU` for detailed
timing simulation.

The qualified configuration is:

- RV64, one thread per CPU, with IMAFDC, Zicsr, Zifencei, Zba, Zbb, Zbs, and
  Sv39/Svnapot;
- machine, supervisor, and user privilege modes;
- classic `atomic_noncaching`, or Ruby CHI with SimpleNetwork;
- one whole-system JitCPU-to-O3 switch, host-driven or guest-coordinated,
  tested on one and sixteen harts; and
- a 16-hart CHI 4x4 SimpleNetwork mesh with 16 RNFs, 16 HNFs, four SNFs, four
  interleaved memory controllers, one miscellaneous node, and one I/O RNI.

Switching back from a detailed CPU to JitCPU is not supported: entering
uncached execution would first require writing back and invalidating the
complete coherent timing hierarchy, which is future work at the Ruby level.
The switch is one way, JitCPU to O3.

JitCPU is not a timing model. It charges one simulated cycle per translated
guest instruction and performs functional physical-memory accesses. Its
reported IPC is therefore exactly 1 by construction; host instructions per
second and wall time are the meaningful performance measures.

### 1.1 Measured expectations

Measurements on 2026-07-28 used an Intel Core Ultra 7 265K host, `gem5.opt`,
the pinned QEMU revision in `ext/qemu/repo`, a 1 GHz simulated CPU, 256 MiB of
RAM, the lazy CLINT timer, and a 65,536-instruction batch. A single RV64
Linux hart reached its first userspace instruction after 343,482,657 guest
instructions:

| Memory system | gem5 host time | Host rate |
| --- | ---: | ---: |
| Classic `atomic_noncaching` | 1.22 s | 281 Minst/s |
| Ruby CHI, one RNF/HNF/SNF | 1.30 s | 264 Minst/s |

Runs varied by less than 3% in host time, and every run of a configuration
retires a bit-identical instruction count. These values characterize that
host and workload, not a performance guarantee. The class default is a
10,000-instruction batch; smaller batches improve event responsiveness at
some host-performance cost.

The 16-hart Linux workload boots entirely on JitCPU, observes all 16 CPUs
online, runs the first dining-philosophers phase, switches to O3, and
validates the second phase on the timing hierarchy, with no timing CHI
traffic in the JitCPU phase and every RNF and memory controller active in
the O3 phase.

## 2. Design goals and non-goals

The implementation must:

1. reuse upstream QEMU translation and execution rather than introduce a
   second RISC-V decoder;
2. keep gem5's `ThreadContext` as the architectural handoff contract;
3. preserve event, interrupt, MMIO, and pseudo-instruction semantics well
   enough to boot Linux; and
4. support whole-system JitCPU-to-O3 takeover.

It does not attempt to provide cache, interconnect, pipeline, contention,
power, or host-parallel CPU timing. It also does not expose JitCPU through
gem5's standard-library `CPUTypes`: configurations instantiate
`RiscvJitCPU` directly so every hart receives an explicit, unique backend
instance ID.

## 3. Source and component layout

```text
gem5 repository
|
+-- src/cpu/jit/                  BSD-licensed gem5 CPU, loader, this doc
+-- ext/qemu/repo/                pinned QEMU fork with JitCPU integration hooks
+-- ext/qemu/gem5-jit/            GPL adapter and smoke test
+-- util/jitcpu/                  reproducible backend and Linux builds
+-- tests/gem5/jitcpu/            configurations and run instructions
+-- tests/test-progs/jitcpu-smoke guest userspace payloads
```

The parent repository's gitlink is the authoritative QEMU fork revision on the
`gem5-jit` branch. The build helper copies `ext/qemu/repo` to a build snapshot;
it never modifies the submodule checkout.

At run time the components interact through a narrow C interface:

```text
                  canonical at switch boundaries
             +--------------------------------------+
             |       gem5 ThreadContext / ISA       |
             +------------------+-------------------+
                                | sync registers, CSRs,
                                | privilege, PC, interrupts
             +------------------v-------------------+
             | RiscvJitCPU (NonCachingSimpleCPU)    |
             | scheduling | stats | MMIO | m5ops    |
             +------------------+-------------------+
                                | dlopen, C interface
             +------------------v-------------------+
             | gem5 QEMU adapter + RISC-V TCG       |
             | one vCPU/hart, single TCG executor   |
             +-------------+------------------------+
                           | translated loads/stores
                 +---------+----------+
                 |                    |
          mapped host RAM       gem5 callbacks
          fast direct path      MMIO/fallback path
```

The gem5 binary has no compile-time QEMU headers or libraries. `USE_JITCPU=y`
builds only the loader and links `libdl`; `backend_path` selects the shared
backend at run time. `USE_JITCPU=n` removes JitCPU and its QEMU dependency.

## 4. Execution model

### 4.1 Backend lifetime and multicore execution

The adapter creates one QEMU RISC-V vCPU for each gem5 JitCPU. Every object
has a unique `backend_instance` in `[0, backend_instance_count)`, and its
`mhartid` is the gem5 context ID. The vCPUs have independent register,
privilege, interrupt, TLB, halted, exception, and LR/SC state, but share one
QEMU runtime and one physical address space.

QEMU runs with `tcg,thread=single`. gem5 releases its event-queue lock while a
batch executes, but the adapter serializes all vCPUs through the one TCG
executor. This matches gem5's event-driven scheduling and is correct for the
qualified multicore workloads; it does not scale execution across host cores.

The adapter calls QEMU's `tcg_cpu_exec()` wrapper, not the low-level
`cpu_exec()` loop, so normal QEMU execution entry/exit synchronization and
translation-block flush handling remain intact.

### 4.2 Batching and event ordering

Each tick imports gem5 state, runs at most `batch_size` translated
instructions, exports state, accounts instructions, and schedules the next
tick. The budget is shortened by:

- the next gem5 instruction-count event;
- the next non-JitCPU event on the event queue;
- an interrupt, exception, WFI, m5 pseudo instruction, or MMIO access; and
- a drain request.

Peer JitCPU tick events at the same tick are deliberately ignored when
shortening a batch. Without that distinction, multicore execution degenerates
to one instruction per call. Device and timer events at that tick still force
a one-instruction boundary. WFI harts sleep until the next relevant event
instead of polling every simulated cycle.

One guest instruction advances one gem5 CPU cycle. Statistics are updated in
batches. Consequently, aggregate instruction counts are exact, but privilege
attribution is based on the privilege mode at a batch boundary and should not
be treated as a precise per-instruction profile.

### 4.3 Memory, time, and interrupts

Contiguous writable gem5 physical memory is mapped into QEMU for direct
translated access. Non-contiguous, interleaved, read-only, and MMIO ranges use
gem5 read/write callbacks. An MMIO access terminates the current batch so
pending device effects are serviced before execution resumes.

In Ruby `atomic_noncaching` mode, JitCPU accesses Ruby's canonical functional
backing store. It intentionally bypasses timing caches and the network.
Interleaved DRAM controllers therefore need not form one host-contiguous
mapping.

gem5 owns simulated time and CLINT interrupt state. QEMU SSTC and asynchronous
virtual timers are disabled, and QEMU's RISC-V `time` callback reads gem5's
CLINT time. Machine interrupt-pending bits remain owned by gem5 devices.
Supervisor software, timer, and external pending bits written by guest
firmware are merged back from QEMU so SBI and Linux CSR behavior is preserved.

That merge is a delta, not a wholesale copy. A translated MMIO access runs
gem5 device code inline, and the PLIC recomputes its supervisor external line
from within that access, so a batch can change a supervisor pending bit on
both sides. Only the supervisor bits QEMU actually modified are imported,
which leaves every batch boundary consistent with what the detailed CPU would
have observed at the same point.

Batch length is bounded by the next device event, so a CLINT driven by one
RTC pin event per `mtime` tick caps every batch at one RTC period. The Linux
configuration instead uses the CLINT's lazy timer mode (`Clint.rtc_period`):
`mtime` is computed from the current tick on demand and only `mtimecmp`
deadlines schedule events, with guest-visible values and MTIP edges
tick-identical to the pin-driven implementation. Batches are then bounded by
real timer deadlines and MMIO rather than the RTC period;
`--classic-rtc-events` restores the pin-driven RTC.

### 4.4 Pseudo instructions and failures

The adapter recognizes RISC-V m5 pseudo instructions, returns their function
number, and lets gem5 execute the existing pseudo-instruction implementation.
Any unexpected QEMU exception or executor exit is fatal and reports the QEMU
exception and guest PC; it is never silently interpreted as normal progress.

QEMU reports both a pseudo instruction and a WFI through the same `EXCP_HLT`
executor exit, and distinguishes them by the instruction word the translated
code stored in `env.bins`. QEMU never clears that field, so the adapter clears
it before every batch and additionally requires the vCPU not to be halted.
Otherwise the first WFI executed after any pseudo instruction on the same hart
is reported as a repeat of that pseudo instruction, which both re-runs its side
effects and advances the guest PC past the instruction following the WFI.

A whole-system switch is a request to the simulation script, so exactly one
hart may execute `m5_switch_cpu` per handoff. gem5 does not coalesce
simultaneous exit events: additional harts raising it in the same tick leave
extra `switchcpu` exits queued, which the script observes as spurious
zero-progress phases after the switch it performed.

## 5. ISA and architectural-state contract

`RiscvJitCPU` supplies a scalar RV64 `RiscvISA` by default. Construction fails
for RV32, vector, hypervisor/MHSU, Zicbom, Zicboz, or Smrnmi configurations.
QEMU also disables extensions outside the qualified common subset, including
V, H, SSTC, Zicbom/Zicboz/Zicbop, Zawrs, Zfa, Zbc, Svadu, and Svvptc.

JitCPU and O3 must use ISA-compatible objects. The switch contract covers:

| State | Handoff mechanism |
| --- | --- |
| PC and integer registers | gem5 takeover plus backend synchronization |
| Floating-point registers, FFLAGS, FRM | backend synchronization, including when FS is Off |
| Privilege and core machine/supervisor CSRs | explicit CSR map |
| SATP and address-translation context | CSR transfer plus QEMU TLB/TB invalidation |
| PMP CSRs and decoded PMP rules | CSR transfer plus RISC-V MMU/PMP takeover |
| Interrupt-pending state | gem5 device state plus controlled QEMU merge |
| WFI, exception, m5op, fault, and LR/SC transient state | cleared when JitCPU takes over |

FFLAGS and FRM require direct QEMU helpers: architectural CSR access would
otherwise reject them when `mstatus.FS=Off`, which is a valid switch state.

## 6. CPU takeover

JitCPU-to-O3 begins in `atomic_noncaching`, so the timing hierarchy is empty.
The normal gem5 drain and `BaseCPU` takeover copy architectural state, after
which the memory mode changes to timing:

```text
JitCPU batch stops
      |
      v
drain CPUs/devices/Ruby (hierarchy already empty)
      |
      v
switchOut(JitCPU) -> timing mode -> O3.takeOverFrom(JitCPU)
```

No cache maintenance is required in this direction: JitCPU never installed a
line in any timing cache, so O3 starts against an empty coherent hierarchy
and warms it up itself.

The reverse direction is deliberately unsupported. A drain stops
transactions but leaves clean and dirty lines resident in the timing caches;
entering uncached execution with those copies would permit stale reads and
lost dirty data. Making that safe requires an acknowledged, hierarchy-wide
writeback-and-invalidate of every coherent cache plus a proof of protocol
and network quiescence, which is Ruby-level future work.

## 7. Required gem5 changes

The patch intentionally limits common-code changes to the following hooks:

| Area | Change | Reason |
| --- | --- | --- |
| Build | `USE_JITCPU`, RISC-V-only source selection, `libdl` | keep QEMU optional and dynamically loaded |
| Atomic CPU | make `AtomicSimpleCPU::tick()` virtual | allow the non-caching subclass to batch translated instructions |
| Event queue | predicate-based `anyEventAtOrBefore()` | distinguish peer JitCPU ticks from device/timer events |
| RISC-V MMU/PMP | copy decoded PMP state during takeover | CSR bytes alone do not update the destination PMP cache |
| CLINT | optional lazy timer (`rtc_period`) | remove the per-tick RTC events that capped every batch at one RTC period |
| CustomMesh | reusable 4x4 CHI topology support | qualify the requested 16-core SimpleNetwork system |

## 8. Build and configuration

### 8.1 Ubuntu host packages

On a current Ubuntu host, install:

```sh
sudo apt update
sudo apt install \
  build-essential scons python3-dev zlib1g-dev pkg-config \
  git patch tar meson ninja-build flex bison \
  libglib2.0-dev libpixman-1-dev libfdt-dev libffi-dev \
  gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu \
  device-tree-compiler cpio \
  opensbi bc libelf-dev libssl-dev
```

The last line covers the full-system runs only; see section 8.4.

### 8.2 macOS host packages

On an Apple Silicon macOS host with Homebrew, install:

```sh
brew install \
  scons meson ninja pkgconf glib pixman dtc \
  riscv-gnu-toolchain isl libmpc mpfr
```

The `isl`, `libmpc`, and `mpfr` formulae provide the runtime libraries used by
Homebrew's RISC-V cross compiler.

### 8.3 Build

```sh
git submodule update --init --depth 1 ext/qemu/repo
util/jitcpu/build-qemu-jit.sh

scons setconfig build/RISCV \
  USE_RISCV_ISA=y USE_JITCPU=y \
  PROTOCOL=CHI RUBY_PROTOCOL_CHI=y \
  NUMBER_BITS_PER_SET=128
scons build/RISCV/gem5.opt -j"$(nproc)"
```

On macOS, replace `$(nproc)` with `$(sysctl -n hw.logicalcpu)`.

The helper builds and runs the two-hart QEMU adapter smoke test, then prints
the backend path. It uses `.so` on Linux and `.dylib` on macOS:

```text
build/qemu-jit/libgem5-qemu-jit.so
build/qemu-jit/libgem5-qemu-jit.dylib
```

A configuration must instantiate one `RiscvJitCPU` per hart, set the same
`backend_instance_count` on all of them, and assign unique
`backend_instance` values. The reference configuration is
`tests/gem5/jitcpu/configs/jitcpu_linux.py`.

### 8.4 Linux artifacts for the full-system runs

The full-system and 16-hart runs need a kernel ELF, an OpenSBI bootloader and
an RV64 musl compiler for the static pthread dining-philosophers payload. An
interactive run additionally uses a static BusyBox initramfs and static RV64
`m5` utility. `util/jitcpu/build-linux-image.sh` produces all five:

```sh
util/jitcpu/build-linux-image.sh
```

| Artifact | Passed as |
| --- | --- |
| `build/jitcpu-linux/vmlinux` | `--kernel` |
| `build/jitcpu-linux/fw_jump.elf` | the positional Linux image |
| `build/jitcpu-linux/m5` | installed as `/sbin/m5` in the interactive image |
| `build/jitcpu-linux/busybox-initramfs.cpio` | `--initrd` for an interactive shell |
| `build/jitcpu-linux/bin/riscv64-linux-musl-gcc` | `DINING_CC` for the payload Makefile |

It downloads Linux (6.12 by default) and musl, and configures the kernel as
`defconfig` plus `NR_CPUS=64`, an initramfs, the 8250 console and devtmpfs,
with `RISCV_ISA_V` configured out. Userspace is pinned to `rv64gc`: gem5's
HiFive device tree advertises `rv64imafdc`, JitCPU decodes nothing beyond
that plus Zicsr, Zifencei, Zba, Zbb and Zbs, and Ubuntu's cross compiler
otherwise defaults to a much newer profile. `KERNEL_VERSION`, `MUSL_VERSION`,
`MARCH` and `JOBS` override the defaults. The kernel is the long step;
rerunning the script reuses it. On macOS,
`util/jitcpu/build-linux-image-docker.sh` builds on a case-sensitive Docker
volume and exports the boot artifacts and `m5` utility to
`build/jitcpu-linux/`; Docker is not needed when gem5 boots those files. The
complete interactive command
is documented in `tests/gem5/jitcpu/README.md`.

Any equivalent kernel works. The requirements are RV64 with `NR_CPUS` at or
above the hart count under test, an initramfs, an 8250 console, and no use
of instructions outside JitCPU's set.

## 9. Qualified 16-core topology

The main routers are numbered in row-major order:

```text
 0 --- 1 --- 2 --- 3
 |     |     |     |
 4 --- 5 --- 6 --- 7
 |     |     |     |
 8 --- 9 ---10 ---11
 |     |     |     |
12 ---13 ---14 ---15
```

Each router hosts one RNF and one HNF. Four SNFs/controllers attach at routers
0, 3, 12, and 15; the miscellaneous node attaches at 5 and the I/O RNI at 10.
Horizontal links have weight 1 and vertical links weight 2 for deterministic
XY routing. Startup validation checks the router IDs, all 48 directed
nearest-neighbor links and weights, every controller attachment, a 1 GiB
physical range, 16 DTB CPU nodes, `SimpleNetwork`, `CustomMesh`, and
`NUMBER_BITS_PER_SET=128`.

## 10. Test strategy

Correctness is gated in layers:

| Layer | Coverage |
| --- | --- |
| QEMU smoke | two harts, independent PC/GPR state, `mhartid`, FCSR while FS is Off, execution and invalidation |
| Linux 1 hart | boot to userspace on classic memory and on Ruby CHI; host-driven switch to O3 with userspace progress and CHI traffic in the O3 phase |
| 16-hart mesh Linux | boot on the 4x4 CHI SimpleNetwork mesh, all 16 CPUs online, one guest-coordinated dining-philosophers handoff to O3 with per-worker, affinity, per-RNF and per-controller validation |

Every JitCPU phase must additionally show zero CHI cache accesses on every
RNF and zero traffic at every memory controller, not only zero interconnect
messages. That makes the uncached-execution invariant independent of the
network model in use.

`tests/gem5/jitcpu/README.md` lists the exact commands.

Two properties of the full-system configuration are load-bearing for the
Linux runs. The platform I/O crossbar needs a `BadAddr` default responder,
as in gem5's own `RiscvBoard`, because O3 issues wrong-path speculative
accesses to unmapped addresses and a bare crossbar treats them as fatal. And
the bounded O3 phase must outlast the RTC-interrupt backlog the preceding
JitCPU phase built up: JitCPU advances simulated time far faster than the
guest retires instructions, so a short phase can be spent entirely in the
kernel's timer catch-up and retire no userspace instructions at all. The
`--o3-ticks` argument sizes that window.

## 11. Limitations

- RISC-V RV64 only; no RV32, vector, hypervisor, SSTC, Zicbom/Zicboz, or
  Smrnmi support.
- Functional execution only: JitCPU results cannot be used for cache,
  interconnect, pipeline, IPC, power, or contention studies.
- Switching is one way: a detailed CPU cannot hand back to JitCPU, because
  entering uncached execution requires writing back and invalidating the
  complete coherent timing hierarchy first (Ruby-level future work).
- TCG execution is serialized across guest harts and does not exploit
  host-core parallelism.
- The backend owns one fixed vCPU set and one physical address space for the
  process lifetime. Multiple independent gem5 systems in one process are not
  supported.
- Checkpoint and restore with a live JitCPU backend are currently broken and
  unsupported; SMT and systems larger than 16 harts are not qualified.
- Batch execution makes detailed privilege-mode statistics approximate and
  bounds event responsiveness by the chosen batch size. With the pin-driven
  RTC the effective batch is also capped by the next device event.
- Exactly one hart may request a whole-system switch per handoff. gem5 does
  not coalesce simultaneous exit events.
- The scalar ISA overlap is deliberately narrower than either simulator's
  complete RISC-V support. Workloads must not select unsupported extensions
  at run time.

## 12. Licensing

The gem5 CPU, loader, switching, and test integration are BSD licensed.
The adapter, QEMU patch, and linked QEMU TCG/RISC-V backend are
GPL-2.0-or-later.

Dynamic loading provides build and source-tree isolation; it is not a
guarantee that distributing gem5 together with the backend avoids GPL
obligations. A distribution containing or designed to operate with the
combined backend must preserve applicable notices and corresponding source
and should receive project-specific legal review.
