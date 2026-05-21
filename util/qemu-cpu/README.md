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
 │ musl/busybox   │─▶│ Linux, halts at a    │─▶│ from the QEMU device tree; │
 │ initramfs +    │  │ barrier, dumps RAM / │  │ injects RAM + per-hart     │
 │ OpenSBI +      │  │ regs / CLINT / PLIC /│  │ regs + CLINT/PLIC/UART;    │
 │ /bin/bench     │  │ UART / DTB           │  │ O3/Timing/Atomic continues │
 └────────────────┘  └──────────────────────┘  └────────────────────────────┘
```

## What it does

* **Race-free capture** — the snapshot is taken at an explicit gdb-breakpoint
  barrier (or a console marker for the idle shell), not "shortly after" a
  marker.
* **Full machine state** — guest RAM, every hart's GPRs / FP registers / CSRs,
  and CLINT + PLIC + 8250 UART device state are captured and restored.
* **Interrupt-driven I/O works** — after restore the UART→PLIC→CPU interrupt
  path is live: a restored idle shell is fully interactive and userspace
  console output works normally.
* **Multi-hart** — `--smp N` snapshots and restores every hart.
* **Self-configuring** — the gem5 HiFive board (device addresses, hart count,
  timebase) is derived from the QEMU `virt` device tree.

## Pieces

| Path | Role |
|------|------|
| `scripts/build-image.sh` | Builds a minimal RISC-V Linux image into `images/`. |
| `scripts/qemu-common.sh` | Shared QEMU machine/CPU settings. |
| `scripts/qemu-boot.sh` | Boot the image interactively under QEMU. |
| `scripts/qemu-snapshot.py` | Boot under QEMU, halt at a barrier, capture a snapshot. |
| `scripts/gdb-dump-regs.py` | gdb helper: dump every hart's registers/CSRs. |
| `scripts/qemu-cpu-test.py` | End-to-end test harness (bench + interactive). |
| `bench/bench.c` | The benchmark restored into gem5. |
| `src/arch/riscv/qemu/qemu_snapshot.{hh,cc}` | gem5 `RiscvQemuSnapshotWorkload`. |
| `configs/example/qemu_cpu/restore.py` | gem5 config: HiFive board + restore. |

## Why the constrained ISA

The Ubuntu RISC-V toolchain targets the RVA23 profile (vector, vector crypto,
Zicond, ...), which gem5's RISC-V decoder does not fully implement. Userspace
is therefore built strictly for `rv64gc` (musl + busybox), and QEMU is told to
expose only the extension set gem5 supports (`qemu-common.sh:QEMU_CPU`) so the
kernel's boot-time "alternatives" patching stays inside it.

## Usage

```bash
# 1. Build the minimal RISC-V Linux image (needs the RISC-V cross toolchain)
util/qemu-cpu/scripts/build-image.sh

# 2a. Capture a benchmark snapshot (race-free gdb-breakpoint barrier)
util/qemu-cpu/scripts/qemu-snapshot.py --mode bench --out snapshots/bench

# 2b. Capture an idle-shell snapshot (for interactive restore)
util/qemu-cpu/scripts/qemu-snapshot.py --mode shell --out snapshots/shell

# 3. Restore into gem5 and run on a detailed CPU
build/RISCV/gem5.opt configs/example/qemu_cpu/restore.py \
    --snapshot-dir snapshots/bench --cpu o3
```

A benchmark run ends with `exit @ tick N : m5_exit instruction encountered`
and prints `BENCH-DONE ok sum=...` to gem5's terminal. For an interactive
restore, run gem5 with `--listener-mode=on` and connect to the terminal port
it prints (`m5term localhost <port>`). `--cpu` accepts `atomic`, `timing`,
`o3`, `minor`.

## Tests

```bash
util/qemu-cpu/scripts/qemu-cpu-test.py --test all --cpu atomic,timing,o3
```

Restores the benchmark snapshot (checks the checksum reaches the console) and
the idle-shell snapshot (types a command, checks the restored shell wakes on
the UART interrupt and executes it). All combinations pass; multi-hart
(`--smp 2`) passes too.

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
