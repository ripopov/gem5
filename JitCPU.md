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
- any number of alternating whole-system JitCPU/O3CPU switches tested up to
  21 switches on four harts and five userspace-coordinated switches on 16
  harts; and
- a 16-hart CHI 4x4 SimpleNetwork mesh with 16 RNFs, 16 HNFs, four SNFs, four
  interleaved memory controllers, one miscellaneous node, and one I/O RNI.

JitCPU is not a timing model. It charges one simulated cycle per translated
guest instruction and performs functional physical-memory accesses. Its
reported IPC is therefore exactly 1 by construction; host instructions per
second and wall time are the meaningful performance measures.

### 1.1 Measured expectations

Measurements on 2026-07-22 used an Intel Core Ultra 7 265K host, `gem5.opt`,
the pinned QEMU revision in `ext/qemu/repo`, a 1 GHz simulated CPU, 256 MiB of
RAM, and a 1,024-instruction batch. A single RV64 Linux hart reached an
instrumented BusyBox shell after 411,079,684 guest instructions:

| Memory system | Median host rate | gem5 host time | End-to-end time |
| --- | ---: | ---: | ---: |
| Classic `atomic_noncaching` | 141.2 Minst/s | 2.91 s | 3.17 s |
| Ruby CHI, one RNF/HNF/SNF | 139.4 Minst/s | 2.95 s | 3.94 s |

Five runs varied by less than 1% in host instruction rate. These values
characterize that host and workload, not a performance guarantee. The class
default is a 10,000-instruction batch; smaller batches improve event
responsiveness at some host-performance cost.

On 2026-07-23, the qualified 16-hart Linux workload:

- booted entirely on JitCPU, observed all 16 CPUs online, then switched to O3;
- completed five alternating JitCPU/O3 switches and six dining-philosophers
  phases in 7 minutes 53 seconds;
- preserved and validated 22,528 meals, 191,488 shared atomic updates,
  per-worker checksums, fork ownership/use counts, CPU affinity, and shared
  memory; and
- produced no timing CHI traffic in JitCPU phases while every RNF and memory
  controller was active in every O3 phase.

## 2. Design goals and non-goals

The implementation must:

1. reuse upstream QEMU translation and execution rather than introduce a
   second RISC-V decoder;
2. keep gem5's `ThreadContext` as the architectural handoff contract;
3. preserve event, interrupt, MMIO, and pseudo-instruction semantics well
   enough to boot Linux;
4. support repeated JitCPU-to-O3 and O3-to-JitCPU takeover; and
5. enter `atomic_noncaching` only after Ruby has written back and invalidated
   the complete coherent hierarchy.

It does not attempt to provide cache, interconnect, pipeline, contention,
power, or host-parallel CPU timing. It also does not expose JitCPU through
gem5's standard-library `CPUTypes`: configurations instantiate
`RiscvJitCPU` directly so every hart receives an explicit, unique backend
instance ID.

## 3. Source and component layout

```text
gem5 repository
|
+-- src/cpu/jit/                  BSD-licensed gem5 CPU and dlopen loader
+-- ext/qemu/repo/                pinned, unmodified QEMU git submodule
+-- ext/qemu/gem5-jit/            GPL adapter, smoke test, QEMU patch
+-- util/jitcpu/                  reproducible backend build helper
+-- tests/gem5/jitcpu/            configurations and regression drivers
+-- tests/test-progs/jitcpu-smoke guest correctness workloads
+-- JitCPU.md                     this specification
```

The parent repository's gitlink is the authoritative QEMU revision. The build
helper copies `ext/qemu/repo` to a build snapshot and applies
`ext/qemu/gem5-jit/qemu.patch`; it never modifies the submodule checkout.

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

### 4.4 Pseudo instructions and failures

The adapter recognizes RISC-V m5 pseudo instructions, returns their function
number, and lets gem5 execute the existing pseudo-instruction implementation.
Any unexpected QEMU exception or executor exit is fatal and reports the QEMU
exception and guest PC; it is never silently interpreted as normal progress.

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
| WFI, exception, m5op, fault, and LR/SC transient state | cleared on reverse takeover |

FFLAGS and FRM require direct QEMU helpers: architectural CSR access would
otherwise reject them when `mstatus.FS=Off`, which is a valid switch state.

## 6. CPU takeover

### 6.1 JitCPU to O3

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

No cache maintenance is required in this direction.

### 6.2 O3 to JitCPU

Drain is necessary but insufficient in the reverse direction: it stops
transactions but leaves clean and dirty cache lines resident. Entering
uncached execution with those copies would permit stale reads and lost dirty
data. The required sequence is:

```text
drain
  |
  v
switchOut(O3)
  |
  v
trace every Ruby cache line
  |
  v
issue acknowledged RubyRequestType::FLUSH for each record
  |
  v
prove all caches/directories empty and protocol/network quiescent
  |
  v
re-arm drain; wait for posted memory writes
  |
  v
atomic_noncaching mode
  |
  v
JitCPU.takeOverFrom(O3)
  |
  +-- flush QEMU software TLB
  +-- complete a global TCG TB flush
  +-- clear transient vCPU state
  `-- import the new ThreadContext
```

Translation blocks are invalidated even when SATP is unchanged because O3 may
have modified page tables or executable memory.

## 7. Ruby and CHI maintenance

### 7.1 Trace-driven FLUSH

The implementation reuses Ruby's checkpoint cache trace as the authoritative
worklist:

```text
CacheRecorder record
       |
       v
injecting RN cache -- FlushLine --> address-selected HNF
       |                               |
       | local clean/invalidate        +-- SnpCleanInvalid --> sharers/owner
       |                               +-- dirty data ------> SNF/memory
       |                               `-- wait for all responses
       |                                           |
       <--------------- FlushAck ------------------+
```

For each recorded block:

1. the injection-side CHI cache cleans and invalidates its local subtree;
2. it forwards `FlushLine` to the address-selected HNF;
3. the HNF uses normal snoop, replacement, and writeback machinery to remove
   all upstream copies and write dirty data to memory; and
4. `FlushAck` is returned only after snoop responses, dirty-data movement, and
   downstream write responses complete.

Absent lines and concurrent duplicate FLUSH requests are legal. Existing
per-line TBE serialization prevents premature or conflicting completion. A
clean exclusive owner may answer without data; the HNF still completes the
invalidation correctly.

### 7.2 Quiescence and coverage invariants

Ruby drain now includes the Ruby system itself. It waits for:

- CHI cache, DVM, and memory-controller TBEs and reservations;
- retry, mandatory, trigger, request-ready, snoop-ready, replacement, and
  prefetch queues, including stalled messages;
- pending downstream memory writes; and
- all SimpleNetwork switches and links to become empty.

Trace capture is permitted only after this stable quiescent point, so a line
cannot move between controllers while the worklist is being formed.

After trace playback, every CHI cache and directory must be empty and the
protocol/network must again be quiescent. This is a fail-closed coverage
invariant: a line missed by the trace produces a fatal error instead of being
discarded. A final re-armed drain waits for writes posted by FLUSH to reach
the memory controller before JitCPU resumes.

The implementation does not use a custom HNF directory walker or a private
maintenance-eviction request. `RubyRequestType::FLUSH` remains the protocol
primitive, making the CHI support independently useful for future
architectural cache-management operations.

## 8. Required gem5 changes

The patch intentionally limits common-code changes to the following hooks:

| Area | Change | Reason |
| --- | --- | --- |
| Build | `USE_JITCPU`, RISC-V-only source selection, `libdl` | keep QEMU optional and dynamically loaded |
| Atomic CPU | make `AtomicSimpleCPU::tick()` virtual | allow the non-caching subclass to batch translated instructions |
| Event queue | predicate-based `anyEventAtOrBefore()` | distinguish peer JitCPU ticks from device/timer events |
| RISC-V MMU/PMP | copy decoded PMP state during takeover | CSR bytes alone do not update the destination PMP cache |
| CPU switching | final drain after memory writeback | wait for maintenance-generated posted writes |
| Ruby system | stable quiescence, trace FLUSH, empty-hierarchy check | make timing-to-uncached switching safe and fail closed |
| Ruby network | `isEmpty()` implementations | include in-flight network traffic in drain |
| CHI | acknowledged hierarchy-wide FLUSH states/transitions | clean and invalidate every coherent copy causally |
| Ruby tester | deterministic and duplicate FLUSH traffic | protocol-level regression independent of CPU switching |
| CustomMesh | reusable 4x4 CHI topology support | qualify the requested 16-core SimpleNetwork system |

Garnet receives generic `isEmpty()` accounting so Ruby drain does not become
incorrect when it is selected elsewhere, but JitCPU switching is qualified
only with SimpleNetwork. No Garnet-specific JitCPU test API or automated
Garnet run is part of this patch.

## 9. Build and configuration

### 9.1 Ubuntu host packages

On a current Ubuntu host, install:

```sh
sudo apt update
sudo apt install \
  build-essential scons python3-dev zlib1g-dev pkg-config \
  git patch tar meson ninja-build flex bison \
  libglib2.0-dev libpixman-1-dev libfdt-dev libffi-dev \
  gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu \
  device-tree-compiler cpio
```

The 16-core static pthread payload additionally needs an RV64 musl cross
compiler. Set `--musl-cc` to its executable; this is normally installed from
a musl cross-toolchain rather than Ubuntu's glibc cross package.

### 9.2 Build

```sh
git submodule update --init ext/qemu/repo
util/jitcpu/build-qemu-jit.sh

scons setconfig build/RISCV \
  USE_JITCPU=y PROTOCOL=CHI RUBY_PROTOCOL_CHI=y \
  NUMBER_BITS_PER_SET=128
scons build/RISCV/gem5.opt -j"$(nproc)"
```

The helper builds and runs the two-hart QEMU adapter smoke test, then prints
the backend path, normally:

```text
build/qemu-jit/libgem5-qemu-jit.so
```

A configuration must instantiate one `RiscvJitCPU` per hart, set the same
`backend_instance_count` on all of them, and assign unique
`backend_instance` values. The test configurations are reference examples:

- `tests/gem5/jitcpu/configs/jitcpu_baremetal.py`
- `tests/gem5/jitcpu/configs/jitcpu_linux.py`
- `tests/gem5/jitcpu/configs/jitcpu_16core_mesh.py`

## 10. Qualified 16-core topology

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

## 11. Test strategy

Correctness is gated in layers:

| Layer | Coverage |
| --- | --- |
| QEMU smoke | two harts, independent PC/GPR state, `mhartid`, FCSR while FS is Off, execution and invalidation |
| Focused CHI FLUSH | four requesters, four HNFs, two interleaved controllers, normal traffic, eviction, periodic FLUSH, concurrent duplicate FLUSH |
| Single-hart bare metal | 21 switches; PC, GPR/FPR, CSR, PMP, page tables, self-modifying code, AMO/LR-SC, MMIO, CLINT, m5ops, dirty writeback, clean invalidation |
| Negative control | skips maintenance and must fail with stale-data code 9 |
| Four-hart bare metal | 21 switches; private footprints, barriers, shared AMOs, per-RNF/controller traffic |
| Four-hart WFI/MSIP | 21 switches; sleeping takeover, per-hart interrupt wake/clear/re-arm |
| Linux 1/4 hart | boot on JitCPU, 11/21 switches, pinned userspace progress, mode and traffic checks, kernel-log scan |
| 16-hart mesh bare metal | all harts/RNFs/controllers, state and traffic across JitCPU-to-O3 |
| 16-hart Linux | repeatable boot/handoff and five guest-coordinated alternating switches with dining-philosophers invariants |

The directed tests, not Linux alone, are the primary correctness gate because
they explicitly prove the architectural and cache-maintenance invariants.

Run the focused and 1/4-hart SimpleNetwork suite:

```sh
python3 tests/gem5/jitcpu/run_repeated_switch_regression.py \
  build/RISCV/gem5.opt build/qemu-jit/libgem5-qemu-jit.so \
  --linux-image /path/to/opensbi \
  --linux-kernel /path/to/linux \
  --outdir /tmp/jitcpu-repeated
```

Run the complete 16-hart acceptance suite, which also invokes the preceding
suite unless `--skip-existing-regressions` is explicitly supplied:

```sh
python3 tests/gem5/jitcpu/run_16core_mesh_regression.py \
  build/RISCV/gem5.opt build/qemu-jit/libgem5-qemu-jit.so \
  /path/to/opensbi --kernel /path/to/linux \
  --musl-cc /path/to/riscv64-linux-musl-gcc \
  --linux-runs 3 --workload-switches 5 \
  --timeout-seconds 3600 \
  --outdir /tmp/jitcpu-16core
```

The 16-core driver requires at least two identical Linux runs, validates the
guest terminal markers and final ticks, rejects vector instructions in the
static payload, and then runs the five-switch workload. The nested suite
verifies that every JitCPU phase has zero timing CHI messages and every O3
phase advances every CPU and exercises every expected RNF/controller.

## 12. Limitations

- RISC-V RV64 only; no RV32, vector, hypervisor, SSTC, Zicbom/Zicboz, or
  Smrnmi support.
- Functional execution only: JitCPU results cannot be used for cache,
  interconnect, pipeline, IPC, power, or contention studies.
- TCG execution is serialized across guest harts and does not exploit
  host-core parallelism.
- The backend owns one fixed vCPU set and one physical address space for the
  process lifetime. Multiple independent gem5 systems in one process are not
  supported.
- Reverse switching is qualified only for Ruby CHI with SimpleNetwork. Other
  Ruby protocols must implement equivalent acknowledged FLUSH, quiescence,
  and empty-hierarchy checks.
- Checkpoint restore with a live backend, SMT, DMA racing with maintenance,
  Garnet switching, and systems larger than 16 harts are not qualified.
- Batch execution makes detailed privilege-mode statistics approximate and
  bounds event responsiveness by the chosen batch size.
- The scalar ISA overlap is deliberately narrower than either simulator's
  complete RISC-V support. Workloads must not select unsupported extensions
  at run time.

## 13. Licensing

The gem5 CPU, loader, switching, Ruby, and test integration are BSD licensed.
The adapter, QEMU patch, and linked QEMU TCG/RISC-V backend are
GPL-2.0-or-later.

Dynamic loading provides build and source-tree isolation; it is not a
guarantee that distributing gem5 together with the backend avoids GPL
obligations. A distribution containing or designed to operate with the
combined backend must preserve applicable notices and corresponding source
and should receive project-specific legal review.
