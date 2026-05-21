# gem5 QEMU-snapshot mode (RISC-V) — Detailed Documentation

> Branch: `qemu-cpu-mode`
> Scope: RISC-V 64-bit full-system

This document explains the design of gem5's QEMU-snapshot mode, the engineering
problems solved while building it, its current limitations, and a complete
usage guide. For a short overview see [`README.md`](README.md).

---

## 1. Motivation

Booting Linux on gem5's detailed CPU models (`O3CPU`, `TimingSimpleCPU`) is
slow — minutes to tens of minutes — because every instruction of the boot is
simulated cycle-by-cycle. Yet the boot is almost never the thing under study;
the *benchmark that runs afterwards* is.

The SystemC world solves this with the QEMU **QBox**: QEMU is embedded as a
library and used as a fast CPU model. gem5 has no equivalent. Its closest
mechanism, the KVM CPU, only works when the host and guest ISA match — so it
cannot accelerate a RISC-V guest on an x86 host, the common case.

QEMU-snapshot mode fills that gap: **boot fast under stock QEMU, snapshot the
machine, and restore that snapshot into a gem5 detailed CPU** so that only
the region of interest is simulated in detail.

### Why a "snapshot bridge" and not embedded libqemu

Embedding QEMU as a library (the literal QBox approach) requires a *fork* of
QEMU built as a shared object with a special embedding API — a multi-month
effort, and no such `libqemu` ships on a normal system. Routing QEMU's TCG
memory accesses through gem5's port system would also be invasive and slow.

Instead this implementation is a **snapshot bridge**:

* stock `qemu-system-riscv64` does the booting (nothing custom);
* a snapshot of the full machine state is captured through QEMU's standard
  control interfaces (QMP and the gdbstub);
* a new gem5 *workload* injects that state into a normal gem5 RISC-V
  full-system simulation.

No QEMU modifications, no new QEMU build — only standard interfaces.

---

## 2. Architecture

### 2.1 The three-stage pipeline

```
  build-image.sh          qemu-snapshot.py             restore.py + gem5
 ┌────────────────┐  ┌──────────────────────┐  ┌────────────────────────────┐
 │ Linux kernel   │  │ qemu-system-riscv64   │  │ gem5 RiscvSystem (HiFive)  │
 │ + musl/busybox │─▶│  -M virt boots Linux, │─▶│  RiscvQemuSnapshotWorkload │
 │   initramfs    │  │  halts at a barrier,  │  │  injects RAM + per-hart    │
 │ + OpenSBI      │  │  dumps RAM/regs/CSRs/ │  │  regs + CLINT/PLIC/UART;   │
 │ + testcases    │  │  CLINT/PLIC/UART/DTB  │  │  O3/Timing/Atomic CPU(s)   │
 │                │  │  -> snapshot/         │  │  continue execution       │
 └────────────────┘  └──────────────────────┘  └────────────────────────────┘
       stage 1                 stage 2                     stage 3
```

### 2.2 Stage 1 — the guest image (`build-image.sh`)

Produces, in `<repo>/images/`:

| Artifact | Description |
|----------|-------------|
| `Image` | Raw RISC-V Linux kernel (Linux 6.12, `defconfig`). |
| `vmlinux` | Kernel ELF with symbols. |
| `initramfs.cpio.gz` | musl + busybox root filesystem, plus one `/bin/<name>` per registered testcase. |
| `fw_jump.bin` | OpenSBI M-mode firmware (from the host package). |

Design choices:

* **initramfs, not a disk** — the whole root filesystem lives in guest RAM,
  so a snapshot contains *no virtio block-device state*.
* **`rv64gc` userspace** — see §3.1.
* **OpenSBI lives in guest RAM** — so its M-mode trap handlers (timer, SBI
  calls) are captured by the snapshot and keep working after restore.
* **`/init` is generic** — it reads `qemusnap.test=<name>` from the kernel
  command line and simply runs `/bin/<name>` (or drops to an interactive
  shell for `qemusnap.test=shell`). It contains no per-testcase logic, so a
  new testcase needs no `/init` change.

The set of testcases compiled into the image is *not* hard-coded in
`build-image.sh`. It is driven by the **testcase registry**,
`scripts/testcases.py` (§3.15): the build script asks `testcases.py` for the
list of testcase C sources and compiles each into the initramfs as
`/bin/<name>`. The registered testcases are:

| Binary | Workload |
|--------|----------|
| `/bin/bench` | single-core, CPU-bound matrix multiply (deterministic checksum). |
| `/bin/philo` | multicore dining philosophers — 5 pthreads contending for shared fork mutexes; used to validate SMP snapshots (§3.12). |
| `/bin/syscall` | Linux syscall / kernel exerciser — drives file I/O, `mmap`, `fork`, `nanosleep`, signals, pipes and `poll` to validate kernel and full-system plumbing on a restored snapshot (§3.15). |

### 2.3 Stage 2 — snapshot capture (`qemu-snapshot.py`)

QEMU is driven through three control planes: a UNIX-socket **serial
console**, the **QMP** monitor (pause + dump physical memory) and the
**gdbstub** (the capture barrier + architectural register/CSR read).

`qemu-snapshot.py` is testcase-agnostic: `--test <name>` selects a testcase
from the registry (§3.15) and the registry supplies every testcase-specific
detail (hart count, capture mechanism, barrier symbol, …). There are exactly
two capture mechanisms, and each testcase declares which one it uses:

* **breakpoint capture** — a *gdb breakpoint* on the testcase's
  `snapshot_barrier()` function. QEMU halts at exactly that instruction;
  there is no capture-window timing race (§3.7). Used by every benchmark
  testcase (`bench`, `philo`, `syscall`). The hart count comes from the
  registry — `philo` captures `--smp 4` for a genuine SMP snapshot (§3.12).
* **marker capture** — wait for a string on the serial console, then QMP
  `stop`. Used to snapshot the idle interactive `shell`.

Up front, `qemu-snapshot.py` also dumps the QEMU `virt` **device tree**
(`-machine virt,dumpdtb=`) and parses it (`dtc`) to learn the platform
layout — see §3.8.

The snapshot directory contains:

| File | Contents | How captured |
|------|----------|--------------|
| `ram.bin` | Raw guest DRAM | QMP `pmemsave` |
| `clint.bin` | CLINT MMIO dump (`mtime`/`mtimecmp`/`msip`) | QMP `pmemsave` |
| `plic.bin` | PLIC MMIO dump (priority/enable/threshold) | QMP `pmemsave` |
| `uart.bin` | 8250 UART register dump | QMP `pmemsave` |
| `regs.hart<N>.txt` | Every GPR + FP reg + CSR of hart N | gdbstub |
| `virt.dtb` | The QEMU `virt` device tree | `-machine dumpdtb` |
| `serial.log` | Console transcript | serial socket |
| `meta.json` | Addresses/sizes/hart list + DTB-derived platform | — |

### 2.4 Stage 3 — restore into gem5 (`restore.py` + `RiscvQemuSnapshotWorkload`)

`configs/example/qemu_snap/restore.py` builds a gem5 `RiscvSystem` whose
HiFive platform is configured **from the snapshot's `meta.json`** — CLINT,
PLIC and UART base addresses, DRAM base/size, the hart count and the CLINT
RTC timebase all come from the QEMU device tree rather than being hand-coded
(§3.8). One CPU is created per hart.

The system's workload is the C++ SimObject **`RiscvQemuSnapshotWorkload`**
(`gem5::RiscvISA::QemuSnapshot`, `src/arch/riscv/qemu_snap/qemu_snapshot.{hh,cc}`).
A *workload*'s `initState()` runs after `m5.instantiate()` with full access
to physical memory and the thread contexts. Instead of loading a kernel it:

1. **loads `ram.bin`** into physical memory (`system->physProxy`, chunked);
2. **restores device state** via MMIO writes through the same proxy —
   * CLINT: `mtime`, and per-hart `mtimecmp`/`msip`;
   * PLIC: per-source priority, per-context interrupt-enable bitmaps and
     per-context threshold;
   * UART: `IER`/`LCR`/`MCR` (with the receive-interrupt-enable forced on);
3. **restores every hart** — for each thread context it calls
   `resetThread()`, writes the integer registers, the 32 FP registers, the
   PC, every CSR and the privilege mode from that hart's dump, then
   `activate()`s it.

gem5's chosen CPU model(s) then continue execution exactly where QEMU left
off.

#### CSR restore order and side effects

The register dump uses QEMU/gdb names (`mstatus`, `satp`, `mie`, …); the
workload maps them to gem5 MiscReg indices straight out of gem5's own
`CSRData` table. Three subtleties (each cost a debugging session):

* **PMP CSRs** are written *with side effects* and *before* the privilege
  level is dropped — those writes rebuild the MMU's PMP table and are only
  legal from M-mode (§3.6).
* **Interrupt CSRs** (`mie`/`mip`/`mideleg`/`medeleg`) are written *with side
  effects* so the value also propagates into gem5's interrupt-controller
  state — otherwise a restored guest never takes an interrupt (§3.9).
* **`sstatus`/`sie`/`sip`** are restricted *views* of `mstatus`/`mie`/`mip`
  and are skipped; only the machine-level CSRs are restored.
* gdb names integer register x8 `fp`; gem5 calls it `s0` (§3.5).

#### Memory subsystem: classic or Ruby

`restore.py` builds one of two memory systems behind the CPUs:

* **classic** (default) — a `SystemXBar` with a single `SimpleMemory`. Fast,
  with no cache or coherence modelling; the CPUs share the crossbar with the
  on-chip IO devices.
* **Ruby** (`--ruby`) — gem5's Ruby coherent cache subsystem running the
  **CHI** protocol (Arm AMBA CHI): per-core private L1 (split I/D) and L2
  caches, a set of distributed L3 home nodes, CHI memory nodes with a real
  DRAM controller, all laid out on a **CustomMesh** NoC described by a
  standard CHI NoC config script. The platform's CLINT/PLIC/UART sit on a
  single `piobus` that the Ruby sequencers drive directly.

Both paths are fed the *same* snapshot and `RiscvQemuSnapshotWorkload`; only
the wiring between the CPUs and DRAM differs. See §3.13.

---

## 3. Issues solved along the way

### 3.1 The toolchain emits instructions gem5 cannot decode

Ubuntu's RISC-V cross toolchain targets the RVA23 profile: its default
`-march` includes the vector extension, vector crypto, `Zicond`, `Zfa`, …
gem5's decoder implements the base ISA, `V`, `Zba/Zbb/Zbs` and `Zicbo*` but
not the rest. **Fix:** userspace is built strictly for `rv64gc` against
**musl libc** (the Ubuntu cross-glibc is itself RVA23 and unusable); the
QEMU CPU is constrained so the kernel's boot-time "alternatives" patching
stays inside gem5's set (§3.2).

### 3.2 The kernel's boot-time "alternatives"

The kernel's C code is compiled `rv64imac` regardless of the toolchain
default, but it patches in optimised routines at boot keyed on the CPU's
advertised extensions. **Fix:** the QEMU CPU disables every extension gem5
lacks (`v`, `h`, `sstc`, `zicond`, …) and pins paging to Sv39 — see
`qemu-common.sh:QEMU_CPU`.

### 3.3 `build-image.sh`: `yes | make oldconfig` + `pipefail`

`yes "" | make oldconfig` aborts the build: `yes` gets `SIGPIPE`, `pipefail`
propagates it and `set -e` aborts. **Fix:** feed config from `/dev/null`.

### 3.4 musl: host `/lib` symlink and missing UAPI headers

musl's `make install` tried to symlink into the host `/lib`, and busybox
needs the Linux UAPI headers musl does not ship. **Fix:** `--syslibdir`
inside the build tree, and `make headers_install` from the kernel tree.

### 3.5 gdb calls x8 `fp`, gem5 calls it `s0`

x8 is the RISC-V frame pointer (`s0`/`fp`). QEMU's gdbstub dumps it as `fp`;
gem5's table calls it `s0`. The original workload left x8 at 0 — the
benchmark ran fine (the matmul does not touch x8) until its closing
stack-canary check `ld a5,0(s0)` dereferenced 0. **Fix:** accept either
name for x8.

### 3.6 PMP writes assert M-mode

`ISA::setMiscReg` asserts the privilege is M when a PMP CSR is written.
**Fix:** `resetThread()` leaves the hart in M-mode; PMP CSRs are written
first, with side effects, and the privilege register is set last.

### 3.7 The capture-window race — race-free barrier

The first design snapshotted "shortly after a serial-console marker": QEMU
ran free for the sub-millisecond between the marker and QMP `stop`.

**Fix — breakpoint capture:** an explicit, race-free barrier. Each benchmark
testcase calls a non-inlined `snapshot_barrier()`; `qemu-snapshot.py` resolves
its address (`nm` on the testcase ELF) and sets a *gdb breakpoint* there. QEMU
halts at exactly that instruction. `GdbDriver` keeps gdb attached for the whole
capture (so the VM stays halted while QMP dumps memory) and synchronises the
asynchronous `continue` by appending an `echo <sentinel>` after it and
reading gdb's output until the sentinel appears.

### 3.8 Auto-deriving the gem5 platform from the QEMU DTB

The gem5 HiFive board and the QEMU `virt` machine must agree on device
addresses, the hart count and the timebase. These were hand-matched.

**Fix:** `qemu-snapshot.py` dumps the QEMU device tree
(`-machine virt,dumpdtb=`), parses it with `dtc`, and records the CLINT /
PLIC / UART addresses, DRAM base/size, hart count and timebase in
`meta.json`. `restore.py` configures `system.platform.{clint,plic,uart}
.pio_addr`, the memory range, the RTC frequency and the CPU count from
there — a change on the QEMU side is now followed automatically.

### 3.9 Interrupt-driven I/O — restoring interrupt-controller state

After a restore the guest could run but had **no working interrupts**: the
idle shell never woke on input, and userspace console output never appeared
(kernel `printk` did, because the console driver writes the UART by
polling).

Two distinct causes:

* **PLIC state was not restored.** The kernel had configured the PLIC during
  boot (UART source enabled, priorities, thresholds). gem5's PLIC started
  blank, so the UART interrupt was never routed to a hart. **Fix:** the
  workload replays the PLIC priority/enable/threshold registers from
  `plic.bin`. (Both gem5 and QEMU follow the standard SiFive PLIC layout, so
  the contexts and bitmaps line up.)

* **Interrupt CSRs were restored without side effects.** gem5's interrupt
  controller keeps cached `ie`/`ip` bitsets that are updated by the *side
  effect* of a `mie`/`mip` write. The workload used `setMiscRegNoEffect`, so
  those bitsets stayed zero and `checkInterrupt()` always returned false —
  the PLIC posted the interrupt but the CPU never took it. **Fix:**
  `mie`/`mip`/`mideleg`/`medeleg` are restored *with* side effects.

With both fixed, the UART→PLIC→CPU chain works: a restored idle shell wakes
on a keypress, and userspace `write()` to the console transmits normally.

### 3.10 Letting the interrupt-driven console drain

`write()` to the console returns once the data is *queued*; the 8250 driver
then drains it via TX interrupts. The benchmark called `m5_exit`
immediately, ending the simulation before the queue drained. **Fix:** the
benchmark calls `tcdrain()` to wait for the console to flush before
`m5_exit`. (`m5_exit` is now only a clean end-of-run signal, not a
console-output workaround.)

### 3.11 MinorCPU speculative wrong-path accesses fatal the crossbar

Restoring onto **MinorCPU** aborted almost immediately with
`fatal: Unable to find destination for [0x8:0x10] on system.membus`.

MinorCPU's load/store queue issues load requests *eagerly*, before the load
is known to be on the committed path. A load fetched down a mispredicted
branch can therefore be translated and sent to memory with a not-yet-valid
base register. In M-mode (OpenSBI's trap handlers — entered constantly for
SBI calls and IPIs) translation is the identity map, so a garbage base is
*not* filtered out by a page fault: the request reaches the crossbar with a
nonsense physical address. A bare `SystemXBar` has no port for unmapped
addresses and `fatal()`s — taking down the whole simulation for what is
really a wrong-path access the CPU was about to squash anyway.
AtomicSimpleCPU and TimingSimpleCPU do not speculate; O3CPU squashes such
loads before they reach memory, so only Minor exposed this.

**Fix:** `restore.py` attaches a `BadAddr` responder to the crossbar's
`default` port (`system.membus.default`). Unclaimed addresses now get an
ordinary bad-address *response* instead of aborting the run — the CPU
squashes the wrong-path instruction and the response is discarded, while a
genuine unmapped access still becomes a proper access fault. This mirrors
gem5's standard NoCache hierarchy (`no_cache.py`), which wires up exactly
this responder; `restore.py` builds its memory system by hand and had simply
omitted it.

### 3.12 Validating multicore snapshots — the dining-philosophers benchmark

A single-core CPU-bound benchmark exercises one hart; the other harts, even
in an `--smp N` snapshot, only sit idle. To actually validate SMP restore a
genuinely concurrent workload is needed.

`/bin/philo` is the classic **dining philosophers**: `NPHIL = 5` pthreads
contend for 5 shared fork mutexes for `ROUNDS = 64` meals each, deadlock-free
via the asymmetric fork-ordering solution. Restored onto gem5 it exercises
the parts of an SMP system a single-threaded benchmark never touches:

* **several harts restored at once**, each mid-execution — at capture time
  the snapshot catches the main thread at the barrier, philosopher threads
  running on other harts, and a hart inside the kernel;
* **cross-hart wakeups** — a philosopher blocked on a fork is woken by
  another hart's `futex` wake, which becomes an IPI delivered over the CLINT
  software-interrupt line (`msip`);
* **the SMP scheduler** migrating threads between harts.

The result is **deterministic regardless of interleaving**: every
philosopher eats exactly `ROUNDS` times, and the checksum (sum of the eater's
id over every meal) is fixed. The benchmark validates both and prints
`PHILO-DONE ok meals=320 checksum=640` on the console. The test harness
additionally checks `stats.txt` to confirm *every* hart advanced its cycle
counter — proof the workload ran on all cores, not just hart 0.

This surfaced and fixed §3.11; with that fix the dining-philosophers
scenario passes on all four CPU models (§5.5).

### 3.13 Restoring into the Ruby CHI memory subsystem

The classic restore path puts the CPUs on a `SystemXBar` with a flat
`SimpleMemory` — fine for functional validation, but it models no caches,
no coherence and no realistic DRAM. To study the memory system the snapshot
must be restorable into **Ruby**, gem5's coherent cache subsystem, with a
real DRAM controller and a real interconnect.

`--ruby` switches `restore.py` to build the memory system through gem5's
**CHI** configuration scripts (`configs/ruby/CHI.py` and `CHI_config.py`)
instead of by hand. CHI is the *only* protocol qemu-snap supports: the gem5
RISCV binary is built with several Ruby protocols, and `restore.py` pins
`--protocol CHI` so callers need only pass `--ruby`. CHI builds, per hart, a
request node (RNF) with private L1 (split I/D) and L2 caches; a set of L3
home nodes (HNF); CHI memory nodes (SNF); and a misc node (MN) — all laid
out on a NoC. A few things had to line up:

* **CHI plugs straight into the legacy Ruby entry point.** `restore.py`
  builds the CPUs first (one per hart, as always) and calls
  `Ruby.create_system()`, which dispatches to `CHI.create_system()`.
  CHI returns a list of `CPUSequencerWrapper`s (each wrapping a hart's
  instruction + data sequencer); `restore.py` wires each
  `system.ruby._cpu_ports[i]` to its CPU with `connectCpuPorts()`, exactly
  as for any other protocol.

* **The standard CHI NoC config script.** CHI's `CustomMesh` topology needs a
  NoC config script describing the mesh dimensions and which router each CHI
  node type binds to. `restore.py` defaults `--chi-config` to the standard
  example `configs/example/noc_config/2x4.py` — a 2×4 mesh with the four
  request nodes on routers 1/2/5/6, home nodes alongside them, the memory
  node and main-memory SNF near router 0/4. The four philo harts map one-to-
  one onto the four RNF routers.

* **The platform PIO devices need a home.** With no `SystemXBar`, CLINT/PLIC
  /UART are placed on a single `piobus`, handed to `Ruby.create_system()` so
  each sequencer's PIO request port is wired to it. CPU accesses to a device
  address are routed by the sequencer out to the `piobus`; accesses to DRAM
  go through the CHI cache hierarchy. CHI's IO request node
  (`system.ruby._io_port`) is given a home on the `piobus` too.

* **The snapshot still loads through a functional port.** The workload writes
  `ram.bin` and the device registers through `system->physProxy`. Under Ruby
  that proxy is a `RubyPortProxy`: RAM writes reach memory functionally,
  device-register writes are forwarded to the `piobus`. The injection code in
  `qemu_snapshot.cc` is unchanged.

* **The `piobus` needs a `BadAddr` responder too.** This is §3.11 again: a
  detailed CPU issues wrong-path speculative loads, and under Ruby a stray
  non-memory address is routed by the sequencer onto the `piobus`. A bare
  `IOXBar` `fatal()`s on it. `restore.py` attaches a `BadAddr` responder to
  `system.piobus.default`, exactly as it does for the classic crossbar.

CHI is a full directory/snoop coherence protocol, so it correctly handles
`LR`/`SC` (its L1 controller sets `sc_lock_enabled`) and an O3 CPU's
load/store queue without any of the eviction-notification tweaks a simpler
protocol would need.

### 3.14 Validating the CHI restore — multicore philo on a CHI NoC

The dining-philosophers snapshot (§3.12) is the validation workload here too:
it is restored into an **O3 CPU + Ruby/CHI** on the CustomMesh NoC, once with
the simple network and once with **Garnet**. Each run is checked for the
deterministic `PHILO-DONE ok meals=320 checksum=640` result, that every hart
advanced its cycle counter, and that the CHI/DRAM statistics are populated
and reasonable — L1/L2/L3 demand accesses *and* misses, the L3 home nodes
receiving requests over the NoC, and the DRAM controller serving both reads
and writes. Both configurations pass (§5.5).

### 3.15 Decoupling testcases from infrastructure and from gem5 mode

The first cut of the tooling mixed three concerns that should be independent:

* the **infrastructure** — building the image, capturing a snapshot, driving
  gem5 — had individual testcases hard-coded into it. `qemu-snapshot.py` had a
  `--mode {bench,philo,shell}` choice baked into its argument parser and a
  branch per mode; `build-image.sh` named each benchmark source explicitly and
  generated a per-testcase `/init` dispatch.
* the **gem5 mode** — CPU model and memory system — was tangled into the test
  harness: `qemu-snap-test.py` had a separate `test_bench` / `test_philo` /
  `test_ruby` / `test_interactive` function, and the Ruby path was hard-wired
  to *one* testcase (the dining philosophers) on *one* CPU (O3). There was no
  way to run, say, a single-core benchmark against the CHI memory subsystem.

Adding a testcase therefore meant editing three files plus an `/init`
heredoc, and a testcase could only ever run in the one gem5 mode someone had
wired up for it.

**The fix is a single declarative registry, `scripts/testcases.py`.** A
`TestCase` entry names the workload's C source, its compiler flags, its
default hart count, how to capture it (breakpoint vs. marker barrier) and how
to validate a restored run (a console pass-marker, or interactive). The
registry is the *only* place a testcase is named:

* `build-image.sh` asks `testcases.py sources` which C files to compile and
  builds each as `/bin/<name>`; `/init` became a generic `exec /bin/<name>`.
* `qemu-snapshot.py --test <name>` looks the testcase up and drives the
  capture from its registry entry — no per-testcase branches remain.
* `qemu-snap-test.py` became one generic runner over the cross product of
  three orthogonal axes — testcase (`--test`), CPU model (`--cpu`) and memory
  system (`--mem`: `classic`, `ruby-simple`, `ruby-garnet`). It selects a
  validation strategy from the testcase's `check` field and never names a
  testcase itself. **Any testcase can now be restored in any gem5 mode** —
  the matrix benchmark under Ruby/CHI, the syscall exerciser on Minor, and so
  on are all just points in that cross product.

Adding a testcase is now: drop a self-contained C file in `bench/`, add one
`TestCase(...)` line to `testcases.py`. The infrastructure picks it up with no
further edits.

### 3.16 The syscall testcase — validating the kernel and full-system path

`bench` stresses the CPU pipeline and `philo` stresses SMP; neither exercises
the *kernel* much — both are almost pure compute after their barrier.
`/bin/syscall` (`bench/syscall.c`) fills that gap. After its
`snapshot_barrier()` it drives a battery of system calls and checks each
result:

* file I/O on tmpfs (`open`/`write`/`lseek`/`read`/`fstat`/`unlink`),
* directories (`mkdir`/`chdir`/`getcwd`/`rmdir`),
* anonymous `mmap` — faults in and touches fresh pages, then `munmap`s,
* `pipe` + `poll`,
* `fork` + `waitpid` — process creation and the scheduler,
* `clock_gettime` + `nanosleep` — blocks until a timer interrupt wakes it,
* signal delivery — a `SIGUSR1` handler must run,
* `uname` and the identity syscalls.

Each check has a deterministic pass/fail outcome; the testcase prints one
`  <name> .. ok` line per check and finally `SYSCALL-DONE ok pass=8 total=8`.
It validates the parts of a restored system a pure compute loop never touches:
S-mode trap/return on every `ecall`, demand paging, process creation, the
timer-interrupt path and signal delivery. It passes on every CPU model and in
both classic and Ruby/CHI memory (§5.5).

---

## 4. What works, and limitations

**Working and tested** (see §5.5):

* restore onto AtomicSimpleCPU, TimingSimpleCPU, O3CPU and MinorCPU;
* a CPU-bound benchmark restored mid-run, reporting its result on the
  (interrupt-driven) console;
* a syscall / kernel exerciser restored mid-run — file I/O, `mmap`, `fork`,
  `nanosleep`, signals, pipes and `poll` all behave correctly after restore
  (§3.16);
* a restored idle shell that is fully interactive — it wakes on the UART
  interrupt, echoes input and executes typed commands;
* **multicore** snapshots: a 4-hart SMP dining-philosophers workload captured
  mid-run and restored, with cross-hart `futex`/IPI wakeups and the SMP
  scheduler running threads on every hart — verified on all four CPU models
  (§3.12).
* **Ruby CHI memory subsystem**: the same multicore snapshot restored into an
  O3 CPU + the CHI coherent cache hierarchy (per-core L1+L2, distributed L3
  home nodes, a real DRAM controller) on a CustomMesh NoC, with both the
  simple network and **Garnet** — running to completion with sane CHI/DRAM
  statistics (§3.13–§3.14).

**Limitations:**

* **Secondary harts are restored in their online/idle state.** A hart that
  was *offline* (SBI HSM stopped) at snapshot time is not cold-started; all
  harts present in the snapshot are assumed online.
* **ISA constrained to gem5's decoder set** — userspace is `rv64gc`, the
  QEMU CPU disables `V`, `Zicond`, `H`, `Sstc`, … Workloads needing those
  are out of scope until gem5's decoder catches up.
* **FP yes, vector no** — `f0`–`f31` and `fcsr` are restored; the vector
  registers are not (V is disabled anyway).
* **Platform compatibility** — the DTB supplies addresses, hart count and
  timebase, but gem5's HiFive devices must still be *register-compatible*
  with QEMU `virt`'s (they are: both are SiFive-style CLINT/PLIC/8250).
* **The benchmark still calls `m5_exit`** — now purely to end the simulation
  cleanly once its console output has drained, since otherwise the guest
  would idle in a shell forever.
* **Ruby uses the CHI protocol only.** `restore.py` wires up exactly one
  Ruby protocol — CHI — on a CustomMesh NoC; the other protocols built into
  the binary are not exposed. Ruby also requires a timing CPU
  (`timing`/`o3`/`minor`, not `atomic`).

---

## 5. Usage guide

### 5.1 Prerequisites

A built `build/RISCV/gem5.opt`, and these host packages (Debian/Ubuntu):

```bash
sudo apt install -y \
  gcc-riscv64-linux-gnu libc6-dev-riscv64-cross \
  flex bison bc libelf-dev libssl-dev \
  device-tree-compiler gdb-multiarch expect cpio \
  qemu-system-misc opensbi
```

`device-tree-compiler` (`dtc`) is required — the platform layout is parsed
from the QEMU device tree. `gdb-multiarch` drives the race-free barrier and
reads the architectural state.

### 5.2 Build the guest image

```bash
util/qemu-snap/scripts/build-image.sh
```

Builds Linux, musl, busybox and every registered testcase into `images/`.
The kernel build is the slow step; it is skipped on reruns if `images/Image`
exists. `util/qemu-snap/scripts/qemu-boot.sh` boots the image interactively for
a sanity check (`QEMU_SMP=4 qemu-boot.sh` for a multicore boot).
`scripts/testcases.py list` prints the registered testcases.

### 5.3 Capture a snapshot

`qemu-snapshot.py --test <name>` captures any registered testcase; the hart
count, capture mechanism and barrier all come from `testcases.py`.

```bash
util/qemu-snap/scripts/qemu-snapshot.py --test bench   --out snapshots/bench
util/qemu-snap/scripts/qemu-snapshot.py --test philo   --out snapshots/philo
util/qemu-snap/scripts/qemu-snapshot.py --test syscall --out snapshots/syscall
util/qemu-snap/scripts/qemu-snapshot.py --test shell   --out snapshots/shell
```

Key options:

| Option | Default | Meaning |
|--------|---------|---------|
| `--test` | `bench` | testcase to capture (any name from `testcases.py`) |
| `--out DIR` | `snapshots/snap` | output snapshot directory |
| `--smp N` | `0` | number of harts (`0` = the testcase's registry default) |
| `--mem-mb N` | `256` | guest RAM size |
| `--break-symbol S` | (registry) | (breakpoint capture) breakpoint symbol |
| `--marker STR` | (registry) | (marker capture) console marker |

### 5.4 Restore into gem5

```bash
build/RISCV/gem5.opt configs/example/qemu_snap/restore.py \
    --snapshot-dir snapshots/bench --cpu o3
```

| Option | Default | Meaning |
|--------|---------|---------|
| `--snapshot-dir DIR` | (required) | snapshot from stage 2 |
| `--cpu MODEL` | `timing` | `atomic`, `timing`, `o3` or `minor` |
| `--clock FREQ` | `1GHz` | CPU/system clock |
| `--max-insts N` | `0` | stop after N instructions (0 = unlimited) |
| `--max-ticks N` | `0` | stop after N ticks (0 = unlimited) |
| `--timer-gap N` | `0` | clamp each restored timer to fire ≤ N mtime ticks after mtime (0 = exact); avoids a long idle fast-forward for shell snapshots |
| `--ruby` | off | restore into the Ruby CHI memory subsystem instead of the classic `SystemXBar` + `SimpleMemory` (§3.13) |

With `--ruby`, the CHI / network options also apply (`restore.py` pins the
protocol to CHI and the topology to CustomMesh):

| Option | Default | Meaning |
|--------|---------|---------|
| `--network N` | `simple` | NoC model: `simple` or `garnet` |
| `--chi-config F` | `noc_config/2x4.py` | standard CHI NoC config script for the CustomMesh |
| `--num-dirs N` | `1` | number of CHI memory (SNF) / DRAM controllers |
| `--num-l3caches N` | `4` | number of CHI L3 home nodes (HNF) |
| `--mem-type M` | `DDR3_1600_8x8` | DRAM model for the CHI memory controller |
| `--l1d-size S` / `--l2-size S` / `--l3-size S` | `32KiB` / `256KiB` / `1MiB` | CHI cache sizes |

```bash
# restore the multicore philo snapshot into O3 + Ruby/CHI, Garnet NoC
build/RISCV/gem5.opt configs/example/qemu_snap/restore.py \
    --snapshot-dir snapshots/philo --cpu o3 \
    --ruby --network garnet --timer-gap 100000
```

`restore.py` is testcase-agnostic — it injects whatever snapshot it is given
into whatever CPU/memory mode is requested. A benchmark run ends with
`exit @ tick N : m5_exit instruction encountered` and prints its result to
gem5's terminal (`m5out/.../system.platform.terminal`) — `BENCH-DONE ok
sum=...` for the matrix benchmark, `PHILO-DONE ok meals=320 checksum=640` for
the dining philosophers, `SYSCALL-DONE ok pass=8 total=8` for the syscall
exerciser. The `--cpu` choice (`atomic`/`timing`/`o3`/`minor`) applies to
every hart; `restore.py` reads the hart count from the snapshot's `meta.json`
and builds one CPU per hart automatically.

For an **interactive** restore of a shell snapshot, run gem5 with
`--listener-mode=on` and connect to the terminal port it prints:

```bash
build/RISCV/gem5.opt --listener-mode=on configs/example/qemu_snap/restore.py \
    --snapshot-dir snapshots/shell --cpu timing --timer-gap 200000
# -> "system.platform.terminal: Listening for connections on port 3456"
m5term localhost 3456     # or: telnet localhost 3456
```

### 5.5 The test harness

`qemu-snap-test.py` is generic: it runs the cross product of three independent
axes and reports PASS/FAIL for each combination.

```bash
# default: every testcase, timing CPU, classic memory
util/qemu-snap/scripts/qemu-snap-test.py

# full sweep, capturing any missing snapshot first
util/qemu-snap/scripts/qemu-snap-test.py \
    --test all --cpu atomic,timing,o3,minor --mem all --capture
```

| Option | Default | Meaning |
|--------|---------|---------|
| `--test` | `all` | comma-separated testcase names, or `all` |
| `--cpu` | `timing` | comma-separated CPU models |
| `--mem` | `classic` | comma-separated of `classic` / `ruby-simple` / `ruby-garnet`, or `all` |
| `--snapshot-root DIR` | `snapshots/` | directory holding `snapshots/<testcase>/` |
| `--capture` | off | capture any missing snapshot via `qemu-snapshot.py` |

Every `(testcase, cpu, mem)` triple is one run. How it is validated comes from
the testcase's `testcases.py` entry:

* a **`terminal`** testcase (`bench`, `philo`, `syscall`) must print its
  console pass-marker, exit via `m5_exit`, and have advanced every hart's
  cycle counter in `stats.txt`. Under a Ruby `--mem` config the CHI cache
  hierarchy and DRAM controller must additionally show real traffic (L1/L2/L3
  demand accesses *and* misses, L3 home-node requests, DRAM reads and writes).
* the **`interactive`** testcase (`shell`) is validated by connecting to
  gem5's terminal, typing `echo OUT$((7*9))END` and checking the restored
  shell wakes, executes it and prints `OUT63END` (output ≠ input, so this
  proves *execution*, not just tty echo).

Ruby requires a timing-class CPU, so `(ruby-*, atomic)` combinations are
skipped automatically.

Verified results — all four CPU models, classic memory:

| CPU | bench | philo (4-hart SMP) | syscall | shell |
|-----|-------|--------------------|---------|-------|
| AtomicSimpleCPU | PASS | PASS | PASS | PASS |
| TimingSimpleCPU | PASS | PASS | PASS | PASS |
| O3CPU | PASS | PASS | PASS | PASS |
| MinorCPU | PASS | PASS | PASS | PASS |

Any testcase also restores into the Ruby CHI memory subsystem on the
CustomMesh NoC — e.g. on an O3 CPU with the Garnet network:

| Testcase | result | CHI / DRAM statistics |
|----------|--------|-----------------------|
| philo (4-hart SMP) | PASS | cache acc 343386 / miss 52680, HNF reqs 3959, DRAM rd 4884 / wr 1560 |
| syscall | PASS | cache acc 432256 / miss 59682, HNF reqs 4892, DRAM rd 7218 / wr 141 |

### 5.6 Troubleshooting

| Symptom | Likely cause / fix |
|---------|--------------------|
| snapshot times out at the barrier | image did not boot — run `qemu-boot.sh`; check the cross toolchain |
| restore hangs immediately | config not using `RiscvSystem`, or the timer never fires — try `--timer-gap 200000` |
| `m5_fail` instead of `m5_exit` | the detailed run computed the wrong checksum — a state-restore fidelity bug |
| restored shell ignores input | PLIC/UART state or interrupt CSRs not restored — check the `QemuSnapshot:` log lines |
| gem5 terminal not listening | pass `--listener-mode=on` to `gem5.opt` |
| `Unable to find destination ... on system.membus` | the crossbar's `BadAddr` default responder is missing — see §3.11 |
| `Unable to find destination ... on system.piobus` | (Ruby) the `piobus` `BadAddr` default responder is missing — see §3.13 |
| `This script requires the CHI build` | the gem5 binary lacks the CHI protocol — rebuild with CHI (it is in the default RISCV `MULTIPLE` build) |

---

## 6. Runtime profile

This section gives the end-to-end wall-clock cost of the three-stage
pipeline, measured on a **clean rebuild** — all collaterals (kernel, musl,
busybox, benchmarks, snapshots) deleted, then everything rebuilt from source
and rerun. The profiled stage-3 target is the headline configuration: an
**O3 CPU + Ruby/CHI + Garnet** restore of the 4-hart dining-philosophers
snapshot.

**Measurement host:** Intel Core Ultra 7 265K (20 hardware threads),
91 GiB RAM, Linux; `build/RISCV/gem5.opt` (the multi-protocol RISCV build).
Build times scale with core count (`-j20` here); simulation times are
single-process. Absolute numbers will differ on other hosts — the *ratios*
are the point.

### 6.1 Stage 1 — build the guest image (`build-image.sh`)

| Phase | Wall time |
|-------|-----------|
| Extract sources + build musl libc (`rv64gc`) | ~12 s |
| Build busybox (static, against musl) | ~14 s |
| Assemble initramfs + compile the testcases | ~3 s |
| Build Linux kernel 6.12 (`defconfig`, `Image` + `vmlinux`, `-j20`) | ~57 s |
| Copy OpenSBI firmware | <1 s |
| **Total** | **~87 s** |

* The source tarballs (~150 MiB: Linux 6.12, busybox, musl) were already
  present. A cold first run also downloads them — network-bound, not a build
  cost; `build-image.sh` keeps them so reruns skip the download.
* The **kernel is the long pole** — about two-thirds of the build. It is
  skipped entirely on reruns if `images/Image` already exists.

### 6.2 Stage 1 detail — building the testcases

The `/bin/philo` dining-philosophers app (181 lines of C) compiles in
**~0.05 s** with `musl-gcc -O2 -static -pthread`; the other testcases are the
same. They are a negligible part of the ~3 s initramfs phase above — the cost
of stage 1 is entirely the kernel and the C library, not the workloads.

### 6.3 Stage 2 — boot under QEMU + capture the snapshot

`qemu-snapshot.py --test philo` boots Linux on 4 harts (philo's registry
default) under stock `qemu-system-riscv64`, runs `/bin/philo` to its
`snapshot_barrier()` breakpoint, and dumps guest RAM + every hart's registers
+ CLINT/PLIC/UART state:

> **~1.25 s wall** for the whole capture (boot + barrier + dumps).

**Instructions executed in QEMU:** a deterministic `-icount` boot to the same
barrier retires **~296.5 million instructions** — the complete 4-hart SMP
Linux boot plus the philo setup. (With `-icount` QEMU's `minstret` CSR is an
exact retired-instruction counter and reads ~296.5 M on every hart, since it
exposes the global count.) `-icount` is used *only* for this measurement; the
real capture run does not enable it.

This 296.5 M-instruction boot is exactly the work the snapshot lets gem5
**skip**.

### 6.4 Stage 3 — restore into gem5 (O3 + Ruby/CHI + Garnet)

`restore.py --cpu o3 --ruby --network garnet --timer-gap 100000` on
`snapshots/philo`:

| Phase | Wall time |
|-------|-----------|
| gem5 **startup** (before any instruction is simulated) | ~17.5 s |
| **Simulation** (philo region, barrier → `m5_exit`) | ~5.3 s |
| **Total gem5 process** | **~22.9 s** |

**Startup** spans process launch through `m5.instantiate()`: loading the
~1.1 GiB multi-protocol `gem5.opt`, importing every SimObject, building the
CHI + Garnet CustomMesh configuration, and injecting the 256 MiB snapshot
(RAM + device state) in the workload's `initState()`. It was measured
directly by rerunning with `--max-ticks 1` (build everything, simulate
almost nothing): **17.5 s**. This is a *fixed* cost — it does not grow with
the length of the region of interest.

**Simulation** runs the dining-philosophers benchmark from the barrier to
`m5_exit`:

* 690,711,000 ticks simulated = **0.691 ms** of guest time;
* **1,066,219 instructions** committed (1,076,075 ops) across the 4 O3 cores;
* per-core cycles — cpu0 517,684 / cpu1 298,460 / cpu2 402,834 /
  cpu3 511,209 (every hart advanced — a genuine multicore run);
* ~2.0×10⁵ simulated instructions per host-second;
* result on the console: `PHILO-DONE ok meals=320 checksum=640`.

The exact instruction/cache/DRAM counts vary slightly between snapshot
captures — SMP boot interleaving is not bit-deterministic, so each capture
freezes the harts in a marginally different state.

### 6.5 Total experiment time

| Step | Wall time |
|------|-----------|
| 1. Build the guest image (kernel + musl/busybox + benchmarks) | ~87 s |
| 2. QEMU boot + snapshot capture | ~1.25 s |
| 3a. gem5 startup (config build + snapshot injection) | ~17.5 s |
| 3b. gem5 simulation (O3 + Ruby/CHI + Garnet) | ~5.3 s |
| **End-to-end total** | **~111 s (~1 min 50 s)** |

The image build (step 1) is a **one-time** cost. Once `images/` exists,
iterating on a detailed run costs only steps 2–3 — about **24 s** per
restore, of which ~17.5 s is the fixed gem5 startup and only ~5.3 s is
detailed simulation.

**Why the snapshot bridge pays off.** QEMU retires the ~296.5 M-instruction
Linux boot in ~1.25 s. gem5's detailed O3 + Ruby/CHI + Garnet model runs at
~2×10⁵ inst/s, so simulating that *same* boot inside gem5 would take roughly
296.5×10⁶ / 2×10⁵ ≈ **25 minutes** — before the benchmark even starts.
QEMU-snapshot mode replaces that with a ~1 s QEMU boot plus a ~17.5 s fixed
restore cost, and spends detailed simulation only on the ~1.07 M-instruction
region of interest.

---

## 7. File reference

| Path | Role |
|------|------|
| `util/qemu-snap/scripts/testcases.py` | the testcase registry — single source of truth |
| `util/qemu-snap/scripts/build-image.sh` | builds the RISC-V Linux image |
| `util/qemu-snap/scripts/qemu-common.sh` | shared QEMU machine/CPU settings |
| `util/qemu-snap/scripts/qemu-boot.sh` | interactive QEMU boot (sanity check) |
| `util/qemu-snap/scripts/qemu-snapshot.py` | capture a snapshot (barrier + DTB + dumps) |
| `util/qemu-snap/scripts/gdb-dump-regs.py` | gdb helper: dump all harts' registers |
| `util/qemu-snap/scripts/qemu-snap-test.py` | generic end-to-end test harness (testcase × CPU × memory) |
| `util/qemu-snap/bench/bench.c` | testcase: single-core matrix benchmark |
| `util/qemu-snap/bench/philo.c` | testcase: multicore dining-philosophers benchmark |
| `util/qemu-snap/bench/syscall.c` | testcase: Linux syscall / kernel exerciser |
| `src/arch/riscv/qemu_snap/qemu_snapshot.{hh,cc}` | `RiscvQemuSnapshotWorkload` |
| `src/arch/riscv/RiscvFsWorkload.py` | the workload SimObject |
| `configs/example/qemu_snap/restore.py` | gem5 restore configuration (classic + Ruby/CHI) |
| `configs/ruby/CHI.py`, `configs/ruby/CHI_config.py` | gem5's standard CHI configuration scripts (used by `--ruby`) |
| `configs/example/noc_config/2x4.py` | standard CHI NoC config script for the CustomMesh |
