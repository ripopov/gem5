# gem5 QEMU-CPU mode (RISC-V) — Detailed Documentation

> Branch: `qemu-cpu-mode`
> Scope: RISC-V 64-bit full-system

This document explains the design of gem5's QEMU-CPU mode, the engineering
problems that were solved while building it, its current limitations, and a
complete usage guide. For a short overview see [`README.md`](README.md).

---

## 1. Motivation

Booting Linux on gem5's detailed CPU models (`O3CPU`, `TimingSimpleCPU`) is
slow — minutes to tens of minutes — because every instruction of the boot is
simulated cycle-by-cycle. Yet the boot is almost never the thing under study;
the *benchmark that runs afterwards* is.

The SystemC world solves this with the QEMU **QBox**: QEMU is embedded as a
library and used as a fast CPU model. gem5 has no equivalent. Its closest
mechanism, the KVM CPU, only works when the host and guest ISA match — so it
cannot accelerate a RISC-V guest on an x86 host, which is exactly the common
case.

QEMU-CPU mode fills that gap: **boot fast under stock QEMU, snapshot the
machine, and restore that snapshot into a gem5 detailed CPU** so that only the
region of interest is simulated in detail.

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
 │ + musl/busybox │─▶│  -M virt boots Linux  │─▶│  RiscvQemuSnapshotWorkload │
 │   initramfs    │  │  to a marker, then    │  │  injects RAM+regs+CLINT,   │
 │ + OpenSBI      │  │  QMP+gdbstub dump the │  │  O3/Timing/Atomic CPU      │
 │ + /bin/bench   │  │  machine -> snapshot/ │  │  continues execution       │
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
| `fw_jump.bin` | OpenSBI M-mode firmware (copied from the host package). |

Design choices:

* **initramfs, not a disk.** The whole root filesystem lives in guest RAM, so
  a snapshot contains *no virtio block-device state* — there is no device
  queue to serialise across the QEMU→gem5 boundary.
* **`rv64gc` userspace.** See §3.1 — userspace is built strictly for the base
  `rv64gc` ISA so every executed instruction is one gem5 decodes.
* **OpenSBI in RAM.** OpenSBI is the M-mode firmware. Because it lives in guest
  RAM it is captured by the snapshot, so its M-mode trap handlers (timer, SBI
  calls) keep working after restore without gem5 modelling them specially.

### 2.3 Stage 2 — snapshot capture (`qemu-snapshot.py`)

QEMU is launched with three control planes wired up simultaneously:

* a **UNIX-socket serial console** — to watch for the marker that says the
  guest has reached the point of interest;
* the **QMP monitor** — to pause the VM and dump physical memory;
* the **gdbstub** — to read the architectural CPU and CSR state.

The driver:

1. connects QMP up front (so the eventual `stop` is a single, sub-millisecond
   command);
2. waits on the serial console for the marker string;
3. issues QMP `stop` the instant the marker appears;
4. dumps state into a snapshot directory.

The snapshot directory contains:

| File | Contents | How captured |
|------|----------|--------------|
| `ram.bin` | Raw guest DRAM (256 MiB @ `0x80000000`) | QMP `pmemsave` |
| `clint.bin` | CLINT MMIO region (`mtime`, `mtimecmp`) | QMP `pmemsave` (MMIO is dispatched too) |
| `plic.bin` | PLIC MMIO region | QMP `pmemsave` |
| `regs.txt` | Every GPR + CSR of the boot hart, `name 0xvalue` | gdbstub via `gdb-dump-regs.py` |
| `serial.log` | Full boot console transcript | serial socket |
| `meta.json` | Base addresses, sizes, paths | written by the driver |

### 2.4 Stage 3 — restore into gem5 (`restore.py` + `RiscvQemuSnapshotWorkload`)

`configs/example/qemu_cpu/restore.py` builds a gem5 `RiscvSystem` whose HiFive
platform deliberately mirrors the QEMU `virt` machine:

| Device | Address | Matches QEMU `virt` |
|--------|---------|---------------------|
| DRAM | `0x80000000` | yes |
| CLINT | `0x02000000` | yes |
| PLIC | `0x0c000000` | yes |
| UART (8250/16550) | `0x10000000` | yes |
| CLINT timebase | 10 MHz RTC | yes |

The system's workload is the new C++ SimObject
**`RiscvQemuSnapshotWorkload`** (`gem5::RiscvISA::QemuSnapshot`,
`src/arch/riscv/qemu/qemu_snapshot.{hh,cc}`). A gem5 *workload* is the right
abstraction: its `initState()` runs after `m5.instantiate()` with full access
to physical memory and the thread contexts. Instead of loading a kernel, it:

1. **loads `ram.bin`** into physical memory through `system->physProxy`
   (chunked `writeBlob`);
2. **restores the boot hart** — for each thread context it calls
   `resetThread()`, then writes the integer registers, PC, privilege mode and
   every CSR from `regs.txt`, then `activate()`s the thread;
3. **seeds the CLINT** — writes `mtime` and `mtimecmp` through the CLINT's
   MMIO interface so the guest's timer keeps ticking.

gem5's chosen CPU model (`--cpu atomic|timing|o3|minor`) then begins fetching
at the restored PC — exactly as if it were resuming from a gem5 checkpoint.

#### CSR name mapping

The register dump uses QEMU/gdb CSR names (`mstatus`, `satp`, `stvec`, …). The
workload builds a name→index map straight out of gem5's own `CSRData` table,
so the two naming schemes line up automatically without a hand-maintained
table. Subtleties handled:

* `sstatus`/`sie`/`sip` are restricted *views* of `mstatus`/`mie`/`mip`; only
  the machine-level CSRs are restored, the views are skipped.
* PMP config/address CSRs are written **with side effects** (so the MMU's PMP
  table is rebuilt) and **before** the privilege level is dropped, because
  those writes are only legal from M-mode (see §3.6).
* gdb names integer register x8 `fp`; gem5 calls it `s0` (see §3.8).

---

## 3. Issues solved along the way

This section documents the non-obvious problems encountered, because they are
the parts most likely to bite anyone extending this work.

### 3.1 The toolchain emits instructions gem5 cannot decode

Ubuntu's RISC-V cross toolchain targets the **RVA23 profile**: its default
`-march` includes the vector extension, vector crypto (`zvbb`, `zvkb`, …),
`zicond`, `zfa`, `zcb`, and more. Even a trivial statically-linked program
contained thousands of vector instructions, and the first boot attempt
panicked with an illegal-instruction trap inside `init` — glibc's
vector-optimised routines.

gem5's RISC-V decoder implements the base ISA, `V`, `Zba/Zbb/Zbs` and
`Zicbo*`, but **not** `Zicond`, the vector-crypto extensions, `Zfa`, `Zcb`,
etc.

**Fix:** build userspace strictly for plain `rv64gc`. The Ubuntu cross-glibc
is itself RVA23 and cannot be used, so userspace is built against **musl
libc** (`musl 1.2.5`) — musl + busybox compiled with `-march=rv64gc
-mabi=lp64d`, statically linked. A build-time check (`objdump | grep`)
asserts the resulting busybox contains zero vector instructions.

### 3.2 The kernel's boot-time "alternatives"

The kernel's *C code* is compiled `rv64imac` regardless of the toolchain
default (the kernel Makefile strips `f`, `d` and `v` from the C `-march`), so
the kernel itself is safe. But the kernel patches in optimised routines at
boot via the **alternatives** mechanism, keyed on the extensions the CPU
*advertises*.

**Fix:** the QEMU CPU is constrained so it only advertises what gem5 supports:

```
-cpu rv64,v=false,h=false,sstc=false,zicond=false,zacas=false,zawrs=false,
         zbc=false,zbkb=false,zfa=false,zfh=false,zfhmin=false,svadu=false,
         sv57=false,sv48=false
```

`sv48`/`sv57` are disabled to pin paging to **Sv39**; `H` (hypervisor) and
`Sstc` are disabled both to shrink the CSR set and to force the timer down the
simple SBI/CLINT path (see §3.5).

### 3.3 `build-image.sh`: `yes | make oldconfig` + `pipefail`

The build script runs `set -euo pipefail`. `yes "" | make oldconfig` fails:
`make` finishes and closes the pipe, `yes` gets `SIGPIPE` and exits non-zero,
`pipefail` propagates that, and `set -e` aborts the build mid-way.

**Fix:** feed config input from `/dev/null` instead of `yes`.

### 3.4 musl: host `/lib` symlink and missing UAPI headers

`make install` for musl tried to create the dynamic-linker symlink in the
host's `/lib` (permission denied), and busybox failed to compile because musl
does not ship the Linux UAPI headers (`linux/kd.h`, …).

**Fix:** configure musl with `--syslibdir` pointing inside the build tree, and
`make headers_install` the Linux UAPI headers from the kernel tree into musl's
include directory.

### 3.5 CLINT requires a `RiscvSystem`, not a plain `System`

The first restore hung forever. gem5's RISC-V CLINT does
`dynamic_cast<RiscvSystem*>(system)` in `init()`; with a plain `System` it
prints *"Set Clint to RiscvSystem failed"* and cannot deliver timer
interrupts to the harts.

**Fix:** `restore.py` builds a `RiscvSystem`.

### 3.6 PMP writes assert M-mode

`RiscvISA::ISA::setMiscReg` asserts the current privilege is `PRV_M` when a
PMP CSR is written. The workload originally restored the privilege level
(Supervisor) before restoring CSRs, so the PMP writes asserted and aborted.

**Fix:** ordering in `applyRegisters()` — `resetThread()` leaves the hart in
M-mode; PMP CSRs are written first (with side effects, while still M-mode),
all other CSRs next, and the privilege register is set **last**.

### 3.7 The idle fast-forward is slow

When a snapshot is restored, the CLINT's `mtime` starts at the captured value
and the next timer fires when it reaches the captured `mtimecmp`. If that gap
is large (the guest scheduled a tick tens of milliseconds away), gem5 must
step the 10 MHz RTC through millions of discrete events before anything
happens.

**Fix:** `restore.py --timer-gap N` clamps `mtimecmp` to at most `mtime + N`
(default 200000). The timer then fires soon after restore; an early timer
interrupt is benign to Linux, which simply reads the clock, finds nothing due,
and reprograms. `--timer-gap 0` disables the clamp for an exact restore.

### 3.8 gdb calls x8 `fp`, gem5 calls it `s0`

This one produced a hard-to-find segfault. The benchmark ran its entire
matrix-multiply correctly, then crashed at the *function epilogue* with a
`SIGSEGV` loading from address 0.

x8 is the RISC-V frame pointer; its ABI names are both `s0` and `fp`. QEMU's
gdbstub dumps it as `fp`, but gem5's register-name table calls it `s0`. The
workload looked up `s0`, did not find it, and **left x8 at 0**. The benchmark
keeps `&__stack_chk_guard` in x8 across the whole function; the matrix
multiply happens to not touch x8, so it ran fine — but the closing
stack-canary check `ld a5, 0(s0)` dereferenced 0.

**Fix:** when restoring x8, accept either name (`s0` or `fp`).

### 3.9 Userspace console output needs interrupts; m5ops do not

After the segfault was fixed the benchmark ran to completion but produced no
output. Kernel `printk` (e.g. a panic) *does* appear, because the kernel
console uses a **polled** UART write. Userspace `write()` to the console goes
through the **interrupt-driven** tty path, which needs the UART→PLIC interrupt
chain — and PLIC state / interrupt routing is not re-established by a snapshot
restore.

**Fix:** the benchmark does not rely on the console. It verifies its own
checksum and signals gem5 directly with an **m5op**: `m5_exit` if the result
is correct, `m5_fail` otherwise. gem5 decodes m5ops regardless of any device
state, giving an unambiguous pass/fail. (See also §4, Limitations.)

---

## 4. Limitations

* **Interrupt-driven I/O is not restored.** A snapshot does not reconstruct
  PLIC routing/enable state or the wakeup path, so interrupt-driven UART
  input/output and a fully interactive restored shell do not work. The
  benchmark is therefore snapshotted *mid-run* (CPU-bound, no I/O needed) and
  reports its result via m5ops. Restoring an idle shell to full interactivity
  is the natural next piece of work.
* **Single hart.** The snapshot and workload restore one hart (`-smp 1`).
  Multi-core would need per-hart register dumps and careful secondary-hart
  bring-up.
* **Floating-point registers are not restored.** `f0`–`f31`/`fcsr` are not
  copied; the integer matmul benchmark does not use them. Add them if a
  workload relies on live FP state across the snapshot.
* **ISA is constrained to what gem5 decodes.** Userspace is `rv64gc`; the QEMU
  CPU is restricted (no `V`, `Zicond`, `H`, `Sstc`, …). Workloads needing
  those extensions are out of scope until gem5's decoder catches up.
* **Capture window.** `qemu-snapshot.py` stops the VM a fraction of a
  millisecond after the marker; the benchmark advances slightly under QEMU
  before the snapshot. This is intentional fast-forwarding and is negligible
  relative to the benchmark length, but it means the gem5 run is not the
  *entire* benchmark to the instruction.
* **Platform must match.** gem5's HiFive board and the QEMU `virt` machine
  must agree on device addresses and the timebase. They do today; changing one
  side requires changing the other.

---

## 5. Usage guide

### 5.1 Prerequisites

A built `build/RISCV/gem5.opt`, and these host packages (Debian/Ubuntu names):

```bash
sudo apt install -y \
  gcc-riscv64-linux-gnu libc6-dev-riscv64-cross \
  flex bison bc libelf-dev libssl-dev \
  device-tree-compiler gdb-multiarch expect cpio \
  qemu-system-misc opensbi
```

| Package(s) | Used for |
|------------|----------|
| `gcc-riscv64-linux-gnu`, `libc6-dev-riscv64-cross` | cross-compile kernel, musl, busybox, bench |
| `flex bison bc libelf-dev libssl-dev` | Linux kernel build |
| `gdb-multiarch` | read CPU/CSR state from QEMU's gdbstub |
| `expect`, `cpio` | console handling / initramfs packing |
| `qemu-system-misc` | provides `qemu-system-riscv64` |
| `opensbi` | provides `fw_jump.bin` |

### 5.2 Build the guest image

```bash
util/qemu-cpu/scripts/build-image.sh
```

Downloads and builds Linux, musl, busybox and the benchmark into `images/`.
The kernel build is the slow step (a few minutes); it is skipped on reruns if
`images/Image` already exists. Useful environment overrides:

| Variable | Default | Meaning |
|----------|---------|---------|
| `IMG` | `<repo>/images` | output directory |
| `KERNEL_VER` | `6.12` | Linux version |
| `QEMU_MEM_MB` | `256` | guest RAM (also set in `qemu-common.sh`) |

Optionally sanity-check the image interactively (exit QEMU with `Ctrl-A x`):

```bash
util/qemu-cpu/scripts/qemu-boot.sh
```

### 5.3 Capture a snapshot

```bash
util/qemu-cpu/scripts/qemu-snapshot.py --out snapshots/bench
```

Boots the image under QEMU, waits for the benchmark's readiness marker, and
writes the snapshot to `snapshots/bench/`. Key options:

| Option | Default | Meaning |
|--------|---------|---------|
| `--out DIR` | `snapshots/snap` | snapshot output directory |
| `--marker STR` | `QEMU-CPU-MODE-BENCH-READY` | console string to snapshot on |
| `--mem-mb N` | `256` | guest RAM size |
| `--settle SEC` | `0` | extra delay after the marker before snapshotting |
| `--boot-timeout SEC` | `120` | give up if the marker never appears |

To snapshot the **idle shell** instead of the benchmark:

```bash
util/qemu-cpu/scripts/qemu-snapshot.py --out snapshots/shell \
    --marker QEMU-CPU-MODE-SHELL-READY
```

### 5.4 Restore into gem5

```bash
build/RISCV/gem5.opt configs/example/qemu_cpu/restore.py \
    --snapshot-dir snapshots/bench --cpu o3
```

| Option | Default | Meaning |
|--------|---------|---------|
| `--snapshot-dir DIR` | (required) | snapshot produced by stage 2 |
| `--cpu MODEL` | `timing` | `atomic`, `timing`, `o3` or `minor` |
| `--clock FREQ` | `1GHz` | CPU/system clock |
| `--max-insts N` | `0` | stop after N instructions (0 = unlimited) |
| `--max-ticks N` | `0` | stop after N ticks (0 = unlimited) |
| `--timer-gap N` | `200000` | clamp the restored timer (see §3.7); 0 = exact |

A successful benchmark run ends with:

```
[restore] exit @ tick NNNN : m5_exit instruction encountered
```

`m5_exit` means the benchmark completed under the detailed CPU **and**
reproduced the expected checksum. `m5_fail` would mean the result was wrong.

### 5.5 Expected results

Verified end-to-end — QEMU boots RISC-V Linux, the snapshot is captured at the
benchmark, and gem5 restores and runs it to a correct checksum:

| CPU model | Outcome |
|-----------|---------|
| `atomic` (AtomicSimpleCPU) | `m5_exit` — checksum correct |
| `timing` (TimingSimpleCPU) | `m5_exit` — checksum correct |
| `o3` (O3CPU) | `m5_exit` — checksum correct |

### 5.6 Troubleshooting

| Symptom | Likely cause / fix |
|---------|--------------------|
| `qemu-snapshot.py` times out waiting for the marker | image did not boot — run `qemu-boot.sh` and inspect; check the cross toolchain is installed |
| Illegal-instruction panic in `init` under QEMU | userspace contains an extension gem5/QEMU-CPU rejects — rebuild the image (§3.1) |
| Restore hangs immediately | config not using `RiscvSystem` (§3.5), or the timer never fires — check `--timer-gap` |
| `m5_fail` instead of `m5_exit` | the detailed run computed the wrong checksum — a state-restore fidelity bug |
| gem5 runs forever after the benchmark | expected for a shell snapshot — the restored CPU goes idle (§4); bound it with `--max-insts` |

---

## 6. File reference

| Path | Role |
|------|------|
| `util/qemu-cpu/scripts/build-image.sh` | builds the RISC-V Linux image |
| `util/qemu-cpu/scripts/qemu-common.sh` | shared QEMU machine/CPU settings |
| `util/qemu-cpu/scripts/qemu-boot.sh` | interactive QEMU boot (sanity check) |
| `util/qemu-cpu/scripts/qemu-snapshot.py` | boot under QEMU + capture a snapshot |
| `util/qemu-cpu/scripts/gdb-dump-regs.py` | gdb helper: dump all registers/CSRs |
| `util/qemu-cpu/bench/bench.c` | the benchmark restored into gem5 |
| `util/qemu-cpu/README.md` | short overview |
| `util/qemu-cpu/DOCUMENTATION.md` | this document |
| `src/arch/riscv/qemu/qemu_snapshot.hh` | `RiscvQemuSnapshotWorkload` declaration |
| `src/arch/riscv/qemu/qemu_snapshot.cc` | snapshot injection logic |
| `src/arch/riscv/RiscvFsWorkload.py` | `RiscvQemuSnapshotWorkload` SimObject |
| `configs/example/qemu_cpu/restore.py` | gem5 restore configuration |
