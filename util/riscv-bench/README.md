# RISC-V functional-CPU benchmarks

Two workloads that bracket the cost of executing RISC-V code on gem5's
functional CPU models (`NonCachingSimpleCPU` by default), the way Spike's
`linux-test/SpikePerf.md` brackets Spike:

| workload | what it exercises |
|---|---|
| CoreMark, bare metal | M-mode, no paging, ~15 KB working set: fetch, decode, execute, load/store and nothing else |
| Linux boot + `ls` | OpenSBI, Linux 6.12 (RVA23 kernel), BusyBox: Sv39/Sv48 paging, TLB and decoder invalidation, traps, timers |

Both run on `configs/example/riscv/noncaching_fs.py`, which enables the
RVA23S64 feature set (V, Zicbom/Zicboz, misaligned access, hypervisor) and
advertises it to the guest through the generated device tree.

## Building the workloads

```sh
util/riscv-bench/build-coremark.sh          # build/riscv-bench/coremark.elf, 5000 iterations
ITERATIONS=200 OUT=build/riscv-bench/coremark-200.elf util/riscv-bench/build-coremark.sh
util/riscv-bench/build-linux-initramfs.sh   # build/riscv-bench/bench-initramfs.cpio
```

CoreMark needs `riscv64-unknown-elf-gcc` with picolibc; the sources are
cloned from `eembc/coremark` unless `COREMARK_SRC` points at a checkout. The
port (`coremark/`) prints through the HiFive UART and exits with `m5_exit`.
The initramfs takes a static RISC-V BusyBox and `m5` binary (`BUSYBOX` and
`M5` point at them); the Linux run also needs an OpenSBI `fw_jump.elf` and
a kernel with 8250 console, initramfs and RISC-V ISA-extension support
(`BOOTLOADER` and `KERNEL`). The numbers in `docs/RiscvNonCachingPerf.md`
use OpenSBI 1.8 and a Linux 6.12 defconfig kernel with V and KVM enabled.

## Running

```sh
scons build/RISCV/gem5.fast -j$(nproc)
util/riscv-bench/bench.sh coremark 3        # three runs, min/mean, checks the CRCs
util/riscv-bench/bench.sh linux 1           # checks the guest reached the end marker
```

`bench.sh` reports MIPS from `simInsts` and `hostSeconds` (simulation only)
and from the wall clock of the whole process (comparable to Spike's
`--stats`, which times from `main()`). `GEM5`, `COREMARK`, `KERNEL`, `INITRD`,
`BOOTLOADER` and `TASKSET` override the defaults; extra arguments go to the
configuration, e.g. `--cpu-type atomic` or `--cpu-type timing` to check that
the other simple CPU models behave the same.

## Verifying a change

A change to the models must not change what the guest does. Besides the
CoreMark CRCs and the Linux end marker, compare `simTicks` and `simInsts`
in the run's `stats.txt` with the previous build; every optimization in the
git history left them identical, and the RVA23 vector/KVM initramfs
(`build/jitcpu-linux-rva23-guest-irq-wait/app-initramfs.cpio`, exit code 0)
covers two-stage translation and the hypervisor extension, where available.

## Profiling

`perf` may be unavailable (kernel.perf_event_paranoid); gperftools works:

```sh
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libprofiler.so CPUPROFILE=/tmp/cm.prof \
  CPUPROFILE_FREQUENCY=2000 build/RISCV/gem5.fast --outdir=/tmp/prof \
  configs/example/riscv/noncaching_fs.py baremetal build/riscv-bench/coremark-200.elf
pprof --text build/RISCV/gem5.fast /tmp/cm.prof | head -50
```

Do not wrap the profiled run in `taskset`: the profiler's timer is inherited
across the exec and kills gem5 before it installs its own handler.
