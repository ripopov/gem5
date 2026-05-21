# gem5 QEMU-CPU mode (RISC-V) — Detailed Documentation

> Branch: `qemu-cpu-mode`
> Scope: RISC-V 64-bit full-system

This document explains the design of gem5's QEMU-CPU mode, the engineering
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

QEMU-CPU mode fills that gap: **boot fast under stock QEMU, snapshot the
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
 │ + /bin/bench   │  │  CLINT/PLIC/UART/DTB  │  │  O3/Timing/Atomic CPU(s)   │
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
| `initramfs.cpio.gz` | musl + busybox root filesystem, plus `/bin/bench`. |
| `fw_jump.bin` | OpenSBI M-mode firmware (from the host package). |

Design choices:

* **initramfs, not a disk** — the whole root filesystem lives in guest RAM,
  so a snapshot contains *no virtio block-device state*.
* **`rv64gc` userspace** — see §3.1.
* **OpenSBI lives in guest RAM** — so its M-mode trap handlers (timer, SBI
  calls) are captured by the snapshot and keep working after restore.
* **`/init` is mode-aware** — with `qemucpu.mode=shell` on the kernel command
  line it drops straight to an interactive shell; otherwise it runs
  `/bin/bench`.

### 2.3 Stage 2 — snapshot capture (`qemu-snapshot.py`)

QEMU is driven through three control planes: a UNIX-socket **serial
console**, the **QMP** monitor (pause + dump physical memory) and the
**gdbstub** (the capture barrier + architectural register/CSR read).

Two capture barriers are supported:

* **`--mode bench`** — a *gdb breakpoint* on the benchmark's
  `snapshot_barrier()` function. QEMU halts at exactly that instruction;
  there is no capture-window timing race (§3.7).
* **`--mode shell`** — wait for a marker on the serial console, then QMP
  `stop`. Used to snapshot the idle interactive shell.

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

`configs/example/qemu_cpu/restore.py` builds a gem5 `RiscvSystem` whose
HiFive platform is configured **from the snapshot's `meta.json`** — CLINT,
PLIC and UART base addresses, DRAM base/size, the hart count and the CLINT
RTC timebase all come from the QEMU device tree rather than being hand-coded
(§3.8). One CPU is created per hart.

The system's workload is the C++ SimObject **`RiscvQemuSnapshotWorkload`**
(`gem5::RiscvISA::QemuSnapshot`, `src/arch/riscv/qemu/qemu_snapshot.{hh,cc}`).
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

**Fix (bench mode):** an explicit, race-free barrier. `bench` calls a
non-inlined `snapshot_barrier()`; `qemu-snapshot.py` resolves its address
(`nm` on the benchmark ELF) and sets a *gdb breakpoint* there. QEMU halts at
exactly that instruction. `GdbDriver` keeps gdb attached for the whole
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

---

## 4. What works, and limitations

**Working and tested** (see §5.5):

* restore onto AtomicSimpleCPU, TimingSimpleCPU and O3CPU;
* a CPU-bound benchmark restored mid-run, reporting its result on the
  (interrupt-driven) console;
* a restored idle shell that is fully interactive — it wakes on the UART
  interrupt, echoes input and executes typed commands;
* multi-hart snapshots (verified with `--smp 2`): every hart's state is
  captured and restored.

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
util/qemu-cpu/scripts/build-image.sh
```

Builds Linux, musl, busybox and the benchmark into `images/`. The kernel
build is the slow step; it is skipped on reruns if `images/Image` exists.
`util/qemu-cpu/scripts/qemu-boot.sh` boots the image interactively for a
sanity check.

### 5.3 Capture a snapshot

```bash
# benchmark snapshot - race-free gdb-breakpoint barrier
util/qemu-cpu/scripts/qemu-snapshot.py --mode bench  --out snapshots/bench

# idle-shell snapshot - for interactive restore
util/qemu-cpu/scripts/qemu-snapshot.py --mode shell  --out snapshots/shell

# multi-hart snapshot
util/qemu-cpu/scripts/qemu-snapshot.py --mode shell --smp 2 --out snapshots/shell2
```

Key options:

| Option | Default | Meaning |
|--------|---------|---------|
| `--mode` | `bench` | `bench` (breakpoint barrier) or `shell` (marker barrier) |
| `--out DIR` | `snapshots/snap` | output snapshot directory |
| `--smp N` | `1` | number of harts |
| `--mem-mb N` | `256` | guest RAM size |
| `--break-symbol S` | `snapshot_barrier` | (bench mode) breakpoint symbol |
| `--marker STR` | `QEMU-CPU-MODE-SHELL-READY` | (shell mode) console marker |

### 5.4 Restore into gem5

```bash
build/RISCV/gem5.opt configs/example/qemu_cpu/restore.py \
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

A benchmark run ends with `exit @ tick N : m5_exit instruction encountered`
and prints `BENCH-DONE ok sum=...` to gem5's terminal
(`m5out/.../system.platform.terminal`).

For an **interactive** restore of a shell snapshot, run gem5 with
`--listener-mode=on` and connect to the terminal port it prints:

```bash
build/RISCV/gem5.opt --listener-mode=on configs/example/qemu_cpu/restore.py \
    --snapshot-dir snapshots/shell --cpu timing --timer-gap 200000
# -> "system.platform.terminal: Listening for connections on port 3456"
m5term localhost 3456     # or: telnet localhost 3456
```

### 5.5 The test harness

`qemu-cpu-test.py` runs both end-to-end tests and reports PASS/FAIL:

```bash
util/qemu-cpu/scripts/qemu-cpu-test.py --test all --cpu atomic,timing,o3
```

* **bench test** — restores `snapshots/bench`, runs the matmul on the
  detailed CPU, and checks `BENCH-DONE ok` appears on the console.
* **interactive test** — restores `snapshots/shell`, connects to gem5's
  terminal, types `echo OUT$((7*9))END`, and checks the restored shell
  wakes, executes it and prints `OUT63END` (output ≠ input, so this proves
  *execution*, not just tty echo).

`--bench-snap` / `--shell-snap` select other snapshot directories (e.g. the
multi-hart `snapshots/shell2`).

Verified results:

| CPU | bench (console output) | interactive (idle shell) |
|-----|------------------------|--------------------------|
| AtomicSimpleCPU | PASS | PASS |
| TimingSimpleCPU | PASS | PASS |
| O3CPU | PASS | PASS |

Multi-hart (`--smp 2`): bench and interactive both PASS.

### 5.6 Troubleshooting

| Symptom | Likely cause / fix |
|---------|--------------------|
| snapshot times out at the barrier | image did not boot — run `qemu-boot.sh`; check the cross toolchain |
| restore hangs immediately | config not using `RiscvSystem`, or the timer never fires — try `--timer-gap 200000` |
| `m5_fail` instead of `m5_exit` | the detailed run computed the wrong checksum — a state-restore fidelity bug |
| restored shell ignores input | PLIC/UART state or interrupt CSRs not restored — check the `QemuSnapshot:` log lines |
| gem5 terminal not listening | pass `--listener-mode=on` to `gem5.opt` |

---

## 6. File reference

| Path | Role |
|------|------|
| `util/qemu-cpu/scripts/build-image.sh` | builds the RISC-V Linux image |
| `util/qemu-cpu/scripts/qemu-common.sh` | shared QEMU machine/CPU settings |
| `util/qemu-cpu/scripts/qemu-boot.sh` | interactive QEMU boot (sanity check) |
| `util/qemu-cpu/scripts/qemu-snapshot.py` | capture a snapshot (barrier + DTB + dumps) |
| `util/qemu-cpu/scripts/gdb-dump-regs.py` | gdb helper: dump all harts' registers |
| `util/qemu-cpu/scripts/qemu-cpu-test.py` | end-to-end test harness |
| `util/qemu-cpu/bench/bench.c` | the benchmark restored into gem5 |
| `src/arch/riscv/qemu/qemu_snapshot.{hh,cc}` | `RiscvQemuSnapshotWorkload` |
| `src/arch/riscv/RiscvFsWorkload.py` | the workload SimObject |
| `configs/example/qemu_cpu/restore.py` | gem5 restore configuration |
