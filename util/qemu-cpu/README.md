# gem5 QEMU-CPU mode (RISC-V)

Booting Linux on a detailed gem5 CPU model is slow. This feature boots Linux
**fast under QEMU**, captures a full machine snapshot, and **restores it into
gem5's O3 / TimingSimpleCPU** so only the benchmark of interest runs under the
detailed timing models.

It is the gem5 analogue of the SystemC "QEMU QBox" idea, implemented as a
*snapshot bridge* rather than an in-process libqemu: stock
`qemu-system-riscv64` does the booting, and a new gem5 workload
(`RiscvQemuSnapshotWorkload`) injects the captured state.

**See [`DOCUMENTATION.md`](DOCUMENTATION.md)** for the full architecture, the
engineering problems solved, limitations, and a detailed usage guide.

```
  build-image.sh        qemu-snapshot.py            restore.py
 ┌──────────────┐  ┌────────────────────┐  ┌────────────────────────┐
 │ kernel +     │  │ QEMU -M virt boots │  │ gem5 HiFive board      │
 │ musl busybox │─▶│ Linux to a shell,  │─▶│ injects RAM+regs+CLINT │
 │ initramfs +  │  │ dumps RAM/regs/CSR/│  │ continues on O3/Timing │
 │ OpenSBI      │  │ CLINT -> snapshot/ │  │ CPU                    │
 └──────────────┘  └────────────────────┘  └────────────────────────┘
```

## Pieces

| Path | Role |
|------|------|
| `scripts/build-image.sh` | Builds a minimal RISC-V Linux image (kernel + musl/busybox initramfs + OpenSBI) into `images/`. |
| `scripts/qemu-common.sh` | Shared QEMU machine/CPU settings. |
| `scripts/qemu-boot.sh` | Boot the image interactively under QEMU (sanity check). |
| `scripts/qemu-snapshot.py` | Boot under QEMU, run to a marker, capture a snapshot. |
| `scripts/gdb-dump-regs.py` | gdb helper that dumps all CPU registers/CSRs. |
| `bench/bench.c` | The benchmark (`/bin/bench`) that runs under gem5 after restore. |
| `src/arch/riscv/qemu/qemu_snapshot.{hh,cc}` | gem5 `RiscvQemuSnapshotWorkload` -- injects the snapshot. |
| `configs/example/qemu_cpu/restore.py` | gem5 config: HiFive board + restore + detailed CPU. |

## How it runs a benchmark

`build-image.sh` builds `/bin/bench` (a deterministic matrix multiply) into the
initramfs; `/init` starts it.  `bench` prints a marker, then enters a long
CPU-bound region.  `qemu-snapshot.py` snapshots the VM the instant that marker
appears, so the snapshot captures `bench` right at the start of that region.
After restore, gem5's detailed CPU executes the matrix multiply purely by
instruction execution and `bench` reports the result with a gem5 `m5op`
(`m5_exit` on the expected checksum, `m5_fail` otherwise) -- m5ops are used
instead of console output because the interrupt-driven UART path is not
re-established by a snapshot restore.

## Why the constrained ISA

The Ubuntu RISC-V toolchain targets the RVA23 profile (vector, vector
crypto, Zicond, ...), which gem5's RISC-V decoder does not fully implement.
So userspace is built strictly for `rv64gc` (musl + busybox), and QEMU is
told to expose only the extension set gem5 supports (`qemu-common.sh`:
`QEMU_CPU`) so the kernel's boot-time "alternatives" patching stays inside
that set.

## Usage

```bash
# 1. Build the minimal RISC-V Linux image (needs the RISC-V cross toolchain)
util/qemu-cpu/scripts/build-image.sh

# 2. Boot under QEMU and capture a snapshot at the benchmark start
util/qemu-cpu/scripts/qemu-snapshot.py --out snapshots/bench

# 3. Restore into gem5 and run the benchmark on a detailed CPU
build/RISCV/gem5.opt configs/example/qemu_cpu/restore.py \
    --snapshot-dir snapshots/bench --cpu o3
```

A successful run ends with:

```
[restore] exit @ tick NNNN : m5_exit instruction encountered
```

`m5_exit` means the benchmark ran to completion under the detailed CPU and
reproduced the expected checksum; `m5_fail` would mean the result was wrong.
`--cpu` accepts `atomic`, `timing`, `o3` and `minor`.

To snapshot the idle shell instead of the benchmark, pass
`--marker QEMU-CPU-MODE-SHELL-READY` to `qemu-snapshot.py`.

## Snapshot format (`snapshots/<name>/`)

| File | Contents |
|------|----------|
| `ram.bin` | Raw guest DRAM image (base `0x80000000`). |
| `clint.bin` | CLINT MMIO dump (`mtime`, `mtimecmp`). |
| `plic.bin` | PLIC MMIO dump. |
| `regs.txt` | Every GPR + CSR of the boot hart (`name 0xvalue`). |
| `serial.log` | Boot console transcript. |
| `meta.json` | Addresses / sizes tying it together. |
