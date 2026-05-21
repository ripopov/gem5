# gem5 QEMU-CPU mode (RISC-V)

Booting Linux on a detailed gem5 CPU model is slow. This feature boots Linux
**fast under QEMU**, captures a full machine snapshot, and **restores it into
gem5's O3 / Timing / Atomic CPU** so only the work of interest runs under the
detailed timing models.

It is the gem5 analogue of the SystemC "QEMU QBox" idea, implemented as a
*snapshot bridge* rather than an in-process libqemu: stock
`qemu-system-riscv64` does the booting, and a new gem5 workload
(`RiscvQemuSnapshotWorkload`) injects the captured state.

**See [`DOCUMENTATION.md`](DOCUMENTATION.md)** for the full architecture, the
engineering problems solved, limitations, and a detailed usage guide.

```
  build-image.sh          qemu-snapshot.py             restore.py + gem5
 ┌────────────────┐  ┌──────────────────────┐  ┌────────────────────────────┐
 │ kernel +       │  │ QEMU -M virt boots   │  │ gem5 HiFive board, built   │
 │ musl/busybox   │─▶│ Linux (SMP), halts   │─▶│ from the QEMU device tree; │
 │ initramfs +    │  │ at a barrier, dumps  │  │ injects RAM + per-hart     │
 │ OpenSBI +      │  │ RAM / regs / CLINT / │  │ regs + CLINT/PLIC/UART;    │
 │ testcases      │  │ PLIC / UART / DTB    │  │ O3/Timing/Atomic/Minor run │
 └────────────────┘  └──────────────────────┘  └────────────────────────────┘
```

## Testcases vs. infrastructure vs. gem5 mode

The three things are deliberately kept separate:

* **Infrastructure** — `build-image.sh`, `qemu-snapshot.py`, `qemu-cpu-test.py`
  — is generic and never names an individual testcase.
* **Testcases** — the guest workloads — live in `bench/` as self-contained C
  files and are declared in one registry, **`scripts/testcases.py`**.
* **gem5 mode** — CPU model and memory system — is chosen at run time, so
  *any testcase can be restored in any mode*.

Adding a testcase is therefore: drop a C file in `bench/`, add one entry to
`testcases.py`. Nothing else changes — the image build compiles it, the
snapshot tool captures it, and the test harness validates it automatically.

## What it does

* **Race-free capture** — the snapshot is taken at an explicit gdb-breakpoint
  barrier (or a console marker for the idle shell), not "shortly after" a
  marker.
* **Full machine state** — guest RAM, every hart's GPRs / FP registers / CSRs,
  and CLINT + PLIC + 8250 UART device state are captured and restored.
* **Interrupt-driven I/O works** — after restore the UART→PLIC→CPU interrupt
  path is live: a restored idle shell is fully interactive and userspace
  console output works normally.
* **Multicore** — `--smp N` snapshots and restores every hart; a 4-hart SMP
  dining-philosophers benchmark restores and runs to completion (cross-hart
  `futex`/IPI wakeups, SMP scheduling) on all four CPU models.
* **Classic or Ruby/CHI memory** — restore into a flat `SystemXBar`/
  `SimpleMemory` (default), or with `--ruby` into the Ruby **CHI** coherent
  cache subsystem: per-core L1+L2 caches, distributed L3 home nodes, a real
  DRAM controller, on a CustomMesh NoC (simple network or **Garnet**), wired
  up by gem5's standard CHI config scripts.
* **Self-configuring** — the gem5 HiFive board (device addresses, hart count,
  timebase) is derived from the QEMU `virt` device tree.

## Pieces

| Path | Role |
|------|------|
| `scripts/testcases.py` | **The testcase registry** — single source of truth. |
| `scripts/build-image.sh` | Builds a minimal RISC-V Linux image into `images/`. |
| `scripts/qemu-common.sh` | Shared QEMU machine/CPU settings. |
| `scripts/qemu-boot.sh` | Boot the image interactively under QEMU. |
| `scripts/qemu-snapshot.py` | Boot under QEMU, halt at a barrier, capture a snapshot. |
| `scripts/gdb-dump-regs.py` | gdb helper: dump every hart's registers/CSRs. |
| `scripts/qemu-cpu-test.py` | Generic end-to-end test harness (testcase × CPU × memory). |
| `bench/bench.c` | Testcase: single-core CPU-bound matrix multiply. |
| `bench/philo.c` | Testcase: multicore dining philosophers (SMP validation). |
| `bench/syscall.c` | Testcase: Linux syscall / kernel exerciser. |
| `src/arch/riscv/qemu/qemu_snapshot.{hh,cc}` | gem5 `RiscvQemuSnapshotWorkload`. |
| `configs/example/qemu_cpu/restore.py` | gem5 config: HiFive board + restore (classic or Ruby/CHI). |

## Why the constrained ISA

The Ubuntu RISC-V toolchain targets the RVA23 profile (vector, vector crypto,
Zicond, ...), which gem5's RISC-V decoder does not fully implement. Userspace
is therefore built strictly for `rv64gc` (musl + busybox), and QEMU is told to
expose only the extension set gem5 supports (`qemu-common.sh:QEMU_CPU`) so the
kernel's boot-time "alternatives" patching stays inside it.

## Usage

```bash
# 1. Build the minimal RISC-V Linux image (needs the RISC-V cross toolchain).
#    Every testcase registered in scripts/testcases.py is compiled in.
util/qemu-cpu/scripts/build-image.sh

# 2. Capture a snapshot of any registered testcase (--test names it; the
#    hart count, capture barrier and so on come from testcases.py).
util/qemu-cpu/scripts/qemu-snapshot.py --test bench   --out snapshots/bench
util/qemu-cpu/scripts/qemu-snapshot.py --test philo   --out snapshots/philo
util/qemu-cpu/scripts/qemu-snapshot.py --test syscall --out snapshots/syscall
util/qemu-cpu/scripts/qemu-snapshot.py --test shell   --out snapshots/shell

# 3. Restore into gem5 and run on a detailed CPU
build/RISCV/gem5.opt configs/example/qemu_cpu/restore.py \
    --snapshot-dir snapshots/philo --cpu o3

# 3b. ...or restore into the Ruby CHI memory subsystem (Garnet NoC)
build/RISCV/gem5.opt configs/example/qemu_cpu/restore.py \
    --snapshot-dir snapshots/philo --cpu o3 \
    --ruby --network garnet --timer-gap 100000
```

`qemu-snapshot.py --list`-equivalent: `scripts/testcases.py list` prints every
registered testcase.

A benchmark run ends with `exit @ tick N : m5_exit instruction encountered`
and prints its result to gem5's terminal — `BENCH-DONE ok sum=...` for the
matrix benchmark, `PHILO-DONE ok meals=...` for dining philosophers. For an
interactive restore, run gem5 with `--listener-mode=on` and connect to the
terminal port it prints (`m5term localhost <port>`). `--cpu` accepts
`atomic`, `timing`, `o3`, `minor`; `restore.py` builds one CPU per hart.

## Tests

`qemu-cpu-test.py` is a generic harness: it runs the cross product of three
independent axes — testcase (`--test`), CPU model (`--cpu`) and memory system
(`--mem`: `classic`, `ruby-simple`, `ruby-garnet`) — so any testcase can be
validated in any gem5 mode.

```bash
# default: every testcase, timing CPU, classic memory
util/qemu-cpu/scripts/qemu-cpu-test.py

# full sweep, capturing any missing snapshot first
util/qemu-cpu/scripts/qemu-cpu-test.py \
    --test all --cpu atomic,timing,o3,minor --mem all --capture

# one testcase, one mode
util/qemu-cpu/scripts/qemu-cpu-test.py --test syscall --cpu o3 --mem ruby-garnet
```

How each run is validated comes from the testcase's `testcases.py` entry: a
`terminal` testcase must print its console pass-marker, exit via `m5_exit` and
have advanced every hart (and, under Ruby, show real CHI/DRAM traffic); the
`interactive` shell testcase is checked by typing a command and confirming it
executes. The harness never names a testcase itself.

## Snapshot format (`snapshots/<name>/`)

| File | Contents |
|------|----------|
| `ram.bin` | Raw guest DRAM image. |
| `clint.bin` | CLINT MMIO dump (`mtime`, `mtimecmp`, `msip`). |
| `plic.bin` | PLIC MMIO dump (priority / enable / threshold). |
| `uart.bin` | 8250 UART register dump. |
| `regs.hart<N>.txt` | Every GPR + FP register + CSR of hart N. |
| `virt.dtb` | The QEMU `virt` device tree. |
| `serial.log` | Console transcript. |
| `meta.json` | Addresses / sizes / hart list / DTB-derived platform. |
