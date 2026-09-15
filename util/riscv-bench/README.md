# RISC-V functional-CPU benchmarks

Two workloads that bracket the cost of executing RISC-V code on gem5's
functional CPU models (`NonCachingSimpleCPU` by default), the way Spike's
`linux-test/SpikePerf.md` brackets Spike:

| workload | what it exercises |
|---|---|
| CoreMark, bare metal | M-mode, no paging, ~15 KB working set: fetch, decode, execute, load/store and nothing else |
| Linux boot + `ls` | OpenSBI, Linux 6.12 without KVM, BusyBox: Sv39/Sv48 paging, TLB and decoder invalidation, traps, timers |

Both run on `configs/example/riscv/noncaching_fs.py`: one RV64 hart with
**MSU** privilege modes (firmware, kernel and applications), VLEN 256 and
misaligned main-memory accesses. **H and guest KVM are disabled.**
The ISA selects supported extensions from RVA23S64; that selection does not
claim complete profile compliance. H remains disabled because this checkout's
timing page-table walker does not support it.

## Building the workloads

```sh
util/riscv-bench/build-coremark.sh
ITERATIONS=200 OUT=build/riscv-bench/coremark-200.elf \
    util/riscv-bench/build-coremark.sh
LINUX_SRC=/path/to/linux-6.12 \
BOOTLOADER=/path/to/fw_jump.elf \
BUSYBOX=/path/to/static-riscv-busybox \
    util/riscv-bench/build-linux.sh
```

CoreMark needs `riscv64-unknown-elf-gcc` with picolibc. Sources are cloned from
`eembc/coremark` unless `COREMARK_SRC` names an existing checkout. The port in
`coremark/` prints through the HiFive UART and exits with `m5_exit`.

`build-linux.sh` replaces the former initramfs-only builder. It builds the
standard static `m5` utility from `util/m5` (or accepts an `M5` build
that supports `workbegin` and `--inst`). It uses an existing Linux source tree
and `riscv64-linux-gnu-` cross compiler, builds `defconfig`
with `CONFIG_VIRTUALIZATION` and `CONFIG_KVM` disabled, and retains vector,
initramfs and serial-console support. Override `CROSS_COMPILE` and `JOBS` as
needed. It leaves source trees unchanged and puts generated files under
`build/riscv-bench/linux/` (or the directory supplied as its first argument):

- `vmlinux` and `kernel.config`: the kernel and its resolved configuration.
- `fw_jump.elf`: the supplied OpenSBI image, jumping to `0x80200000`.
- `initramfs.cpio`: static BusyBox, `m5`, and the benchmark `/init`.

The measurements use Linux 6.12 and OpenSBI 1.8. Firmware may support optional
H hardware, but it boots this guest without H. Kernel build timestamps and
user/host names are fixed for reproducible release strings. Artifact hashes
are recorded with the measurements.

## Running

Use a RISC-V gem5 build with CHI enabled for the Ruby cases:

```sh
scons build/RISCV/gem5.fast -j$(nproc)
COREMARK=build/riscv-bench/coremark-200.elf \
    util/riscv-bench/bench.sh coremark 3 --cpu-type direct
util/riscv-bench/bench.sh linux 1 --cpu-type direct
```

`bench.sh` reports simulation MIPS and whole-process wall time. `GEM5`,
`COREMARK`, `KERNEL`, `INITRD`, `BOOTLOADER` and `TASKSET` override the defaults.
Extra arguments go to the config. The default is cacheless SimpleMemory;
`--cache-hierarchy classic --caches` or `--cache-hierarchy ruby` creates
64 KiB L1I, 64 KiB L1D, 1 MiB L2 and 8 MiB L3. Ruby uses CHI/SimpleNetwork.
`--memory ddr4 --num-mem-ctrls 4` adds four interleaved controllers; 1 and 2
controllers are also supported. Cache capacities have individual size options.
Classic snoop filters have headroom beyond the upstream cache capacities.
These are generic parameters, not a calibrated commercial core.

Use `--cpu-type direct` to select `RiscvDirectMemorySimpleCPU`, which
accesses the existing RAM allocation without traversing the hierarchy.
Eligible backing stores and their owners are discovered automatically;
there is no `direct_memory` parameter or backdoor state. MMIO,
reservation-sensitive stores and special accesses retain atomic packet handling.

The default `--cpu-type noncaching` uses the upstream `NonCachingSimpleCPU`:
instruction fetch can use backdoors; data accesses use packets. The two CPU
classes inherit directly from `AtomicSimpleCPU`. `--cpu-type timing` starts
in timing mode; `--cpu-type atomic` uses classic caches but bypasses Ruby.
The former `--direct-memory` flag has been removed. To exclude a memory
from direct access, set its `kvm_map` parameter to `False`.

## Boot once, then switch to timing

The initramfs writes a file, emits `m5 --inst workbegin 0 0` after boot, then
checks that file, writes 256 KiB, sleeps for a timer wakeup, runs `ls` and
`cpuinfo`, and exits. Normally the work marker does not stop simulation.
`--switch-to-timing` stops there, drains and switches to TimingSimpleCPU, and
continues the same guest. Both CPUs use identical MSU and PMA settings.
The utility uses instruction-based calls (`--inst`), so no m5 MMIO window
is needed.

```sh
build/RISCV/gem5.fast --outdir=/tmp/linux-handoff \
    configs/example/riscv/noncaching_fs.py linux \
    --bootloader build/riscv-bench/linux/fw_jump.elf \
    --kernel build/riscv-bench/linux/vmlinux \
    --initrd build/riscv-bench/linux/initramfs.cpio \
    --cache-hierarchy ruby --memory ddr4 --num-mem-ctrls 4 \
    --cpu-type direct --switch-to-timing
```

For classic, replace `--cache-hierarchy ruby` with
`--cache-hierarchy classic --caches`. This workflow switches only once, from
direct/noncaching to timing. It dumps boot statistics and resets them at
the handoff,
so `stats.txt` contains separate boot and timing sections. Run this command
directly; `bench.sh` expects one statistics section for throughput comparisons.

## Verifying a change

Compare CoreMark CRCs, `simTicks` and `simInsts`, and the Linux completion
markers between direct and noncaching CPU types using the same binary and
artifacts.
Expect cache accesses and CHI messages to remain zero in noncaching mode and
to increase after timing takeover. Timing changes elapsed simulated time, so
its ticks and interrupt-driven instruction counts need not match atomic mode.
The guest checks RAM preservation and timer execution on either path.
See `docs/RiscvNonCachingPerf.md` for results and the validation record.

## Profiling

`perf` may be unavailable (kernel.perf_event_paranoid); gperftools works:

```sh
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libprofiler.so CPUPROFILE=/tmp/cm.prof \
  CPUPROFILE_FREQUENCY=2000 build/RISCV/gem5.fast --outdir=/tmp/prof \
  configs/example/riscv/noncaching_fs.py baremetal \
  build/riscv-bench/coremark-200.elf --cpu-type direct
pprof --text build/RISCV/gem5.fast /tmp/cm.prof | head -50
```

Do not wrap the profiled run in `taskset`: the profiler's timer is inherited
across the exec and kills gem5 before it installs its own handler.

## Multicore correctness tests

The [bare-metal multicore suite](multicore/README.md) exercises reservations,
AMO/LR-SC contention, producer-consumer handoffs and instruction publication
with 2/4/8 harts. It also covers interleaved memory and repeated CPU switches.
