# AGENTS.md

gem5 with `RiscvJitCPU` — a QEMU/TCG-backed RISC-V CPU model that boots Linux at
~280 Minst/s and hands the architectural state to `RiscvO3CPU` for detailed
simulation. Branch: `riscv-jit-cpu-split`.

Reference documentation, in increasing depth:

- [`JITCPU_TUTORIAL.md`](JITCPU_TUTORIAL.md) — how the model works, and the boot benchmark
- [`src/cpu/jit/README.md`](src/cpu/jit/README.md) — the model's spec, build matrix, limitations
- [`tests/gem5/jitcpu/README.md`](tests/gem5/jitcpu/README.md) — the regression entry points
- [`docs/jitcpu-deck/`](docs/jitcpu-deck/) — the 7-slide talk this demo accompanies

## Live demo: Linux boot on JitCPU, then hand off to O3

This is the runbook for the talk in [`docs/jitcpu-deck/`](docs/jitcpu-deck/):
from a fresh clone to a Linux boot that finishes in under three seconds and
switches to `RiscvO3CPU` at the first userspace instruction.

Do steps 0–4 **before** the talk. Only step 5 runs live; it takes about three
seconds. Steps 2–4 together take roughly an hour on a 20-core host, download
~150 MB and leave ~10 GB in `build/`.

### 0. Host packages

Ubuntu (the full list, kept in
[`src/cpu/jit/README.md` §9.1](src/cpu/jit/README.md#91-ubuntu-host-packages)):

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

macOS with Homebrew is covered in
[§9.2](src/cpu/jit/README.md#92-macos-host-packages); everything below runs
unchanged except that the backend library comes out as `.dylib`.

### 1. Clone and pin the QEMU submodule

```sh
git clone -b riscv-jit-cpu-split https://github.com/ripopov/gem5 gem5-riscv-jit-cpu
cd gem5-riscv-jit-cpu
git submodule update --init --depth 1 ext/qemu/repo
```

The gitlink is the authoritative QEMU revision — `git submodule update` without
`--remote`, always. The submodule checkout stays pristine: the build helper
snapshots it elsewhere and applies
[`ext/qemu/gem5-jit/qemu.patch`](ext/qemu/gem5-jit/qemu.patch) to the snapshot,
so `git submodule status` stays meaningful and every gem5-owned change to QEMU
is visible in this repository. Never patch `ext/qemu/repo` in place.

### 2. Build the QEMU backend (~5 min)

```sh
util/jitcpu/build-qemu-jit.sh          # -> build/qemu-jit/libgem5-qemu-jit.so
```

The helper ends by running `gem5-qemu-jit-smoke`, which brings up two harts and
checks independent register state, execution and translation invalidation. If
that passes, the backend is good.

Rerun it after any change under `ext/qemu/gem5-jit/`. gem5 `dlopen`s the library
by path and will happily load a stale one — the resulting failures look like CPU
model bugs, not a stale build. If the pinned revision or the patch changed, the
helper refuses to reuse the snapshot and tells you to remove
`build/qemu-jit` and `build/qemu-jit.qemu-source` first.

### 3. Build gem5 (~40 min from cold)

```sh
scons setconfig build/RISCV \
  USE_RISCV_ISA=y USE_JITCPU=y \
  PROTOCOL=CHI RUBY_PROTOCOL_CHI=y \
  NUMBER_BITS_PER_SET=128
scons build/RISCV/gem5.opt -j"$(nproc)"
```

`USE_JITCPU=y` is what compiles the model in; `PROTOCOL=CHI` is what `--ruby-chi`
needs at run time, and `NUMBER_BITS_PER_SET=128` is what the 16-hart mesh configs
demand (harmless for the single-hart demo, and the configs refuse to start
without it).

### 4. Linux artifacts (~15 min, downloads ~150 MB)

```sh
util/jitcpu/build-linux-image.sh       # -> build/jitcpu-linux/
```

It downloads Linux 6.12 and musl 1.2.5, then writes:

| Artifact | Role in the demo |
| --- | --- |
| `build/jitcpu-linux/fw_jump.elf` | OpenSBI bootloader — the positional image argument |
| `build/jitcpu-linux/vmlinux` | kernel ELF — `--kernel` |
| `build/jitcpu-linux/bin/riscv64-linux-musl-gcc` | rv64gc-pinned musl compiler; only the 16-hart workloads need it |

`fw_jump.elf` is copied from the distro's `opensbi` package, so nothing is
compiled for it. The kernel is `defconfig` plus `NR_CPUS=64`, initramfs, 8250
console and devtmpfs, with `RISCV_ISA_V` configured out — gem5's device tree
advertises `rv64imafdc` and JitCPU decodes nothing beyond that plus
Zicsr/Zifencei/Zba/Zbb/Zbs. Building the kernel is the long step; rerunning the
script reuses it. Override with `KERNEL_VERSION`, `MUSL_VERSION`, `MARCH` or
`JOBS`.

The initramfs is checked in
([`jitcpu-linux-init.cpio`](tests/test-progs/jitcpu-smoke/src/jitcpu-linux-init.cpio)),
so there is nothing to build for userspace. Its `/init` is a handful of
instructions that execute `m5_exit`; that instruction is the boot marker the
demo switches on.

### 5. The live run (~3 s)

```sh
build/RISCV/gem5.opt tests/gem5/jitcpu/configs/jitcpu_linux.py \
    build/jitcpu-linux/fw_jump.elf build/qemu-jit/libgem5-qemu-jit.so \
    --kernel build/jitcpu-linux/vmlinux \
    --initrd tests/test-progs/jitcpu-smoke/src/jitcpu-linux-init.cpio \
    --ruby-chi --switch-to-o3
```

Measured on a Core Ultra 7 265K, 1 hart @ 1 GHz, 256 MiB, HiFive platform:

```
JitCPU Linux stopped @ tick 461950568000: m5_exit instruction encountered
Linux boot phase: jit per-core instructions 343482657
Linux boot phase: Ruby CHI carried 0 messages
Linux boot phase: per-RNF CHI cache accesses 0
Linux boot phase: per-controller memory bytes 0
switching cpus
Switched JitCPU -> O3CPU @ tick 461950568000, memory mode enum_MemoryMode.timing
O3CPU continued @ tick 461960568000: simulate() limit reached
O3 takeover phase: o3 per-core user instructions 1
O3 takeover phase: Ruby CHI carried 6144 messages
O3 takeover phase: per-RNF CHI cache accesses 1324
O3 takeover phase: per-controller memory bytes 13824

real    0m2.9s
```

What to point at, in order:

1. **343 M instructions in ~2.9 s of wall clock**, 0.462 s of simulated guest
   time — a full Linux boot, only ~2.6× slower than real time.
2. **The boot phase carried zero CHI messages.** JitCPU's accesses go straight to
   the backing store; the coherent hierarchy exists but is untouched. That is the
   trade being made, visible as a number.
3. **The switch happens at the same tick the boot ended** — one
   `m5.switchCpus()`, memory mode flips `atomic_noncaching` → `timing`.
4. **The O3 phase immediately generates CHI traffic** from the state JitCPU
   handed over. Same kernel, same registers, now in a detailed model.

Every run retires a bit-identical instruction count — the simulation is
deterministic, so 343,482,657 is reproducible on any host and only wall time
varies. The guest console is written to
`m5out/system.platform.terminal`; `tail` it to show the real kernel log if
someone asks whether Linux truly booted.

### Optional: the baseline that makes the number mean something (~100 s)

Same config file, same platform, same memory mode — only `--cpu` differs:

```sh
build/RISCV/gem5.opt tests/gem5/jitcpu/configs/jitcpu_linux.py \
    build/jitcpu-linux/fw_jump.elf build/qemu-jit/libgem5-qemu-jit.so \
    --kernel build/jitcpu-linux/vmlinux \
    --initrd tests/test-progs/jitcpu-smoke/src/jitcpu-linux-init.cpio \
    --cpu noncaching
```

`RiscvNonCachingSimpleCPU` boots the same kernel in ~98 s against JitCPU's ~1.6 s
(classic memory, no `--ruby-chi`): **61.7× end-to-end**, 80.6× on gem5 host time.
Start this one in a second terminal at the beginning of the talk and come back to
it — it is still running when the JitCPU slide is done.
Full table: [`JITCPU_TUTORIAL.md` §8.2](JITCPU_TUTORIAL.md#82-results).

### Optional: switching back, repeatedly (~3.5 s)

The one-way handoff above is the headline. `--repeated-switches` shows the harder
direction — O3 back to JitCPU, which needs the hierarchy-wide CHI flush:

```sh
build/RISCV/gem5.opt tests/gem5/jitcpu/configs/jitcpu_linux.py \
    build/jitcpu-linux/fw_jump.elf build/qemu-jit/libgem5-qemu-jit.so \
    --kernel build/jitcpu-linux/vmlinux \
    --initrd tests/test-progs/jitcpu-smoke/src/jitcpu-linux-init.cpio \
    --ruby-chi --repeated-switches 3
```

It boots, then alternates JIT → O3 → JIT → O3 for bounded intervals, checking
after each phase that the active model made progress and that only O3 touches
CHI, and ends with `Linux repeated-switch validation passed after 3 switches`.
Reverse switching is qualified for Ruby CHI + SimpleNetwork only.

### Useful knobs during Q&A

| Flag | Effect |
| --- | --- |
| `--cpu jit\|noncaching` | JitCPU or `RiscvNonCachingSimpleCPU`, same everything else |
| `--ruby-chi` | Ruby CHI hierarchy instead of the classic memory system |
| `--batch-size N` | guest instructions per JitCPU event (default 65536) |
| `--classic-rtc-events` | one CLINT event per `mtime` tick — reproduces the 3.7× slowdown of [§8.3](JITCPU_TUTORIAL.md#83-finding-and-fixing-the-bottleneck) |
| `--o3-ticks N` | how long O3 runs after the handoff (default 10 M ticks = 10 µs) |
| `--num-cpus N` | more harts (TCG is serialized across them) |
| `--outdir DIR` | keep the previous run's stats and console log around |

`--classic-rtc-events` is the good one to have loaded: it turns the "why was
this only 22× before" story into a number the audience watches appear.

### If something fails on stage

| Symptom | Cause |
| --- | --- |
| `--ruby-chi requires a gem5 binary built with PROTOCOL=CHI` | step 3 was run without `PROTOCOL=CHI RUBY_PROTOCOL_CHI=y` |
| `QEMU is not initialized at ext/qemu/repo` | step 1's `git submodule update --init` was skipped |
| `The QEMU revision or integration patch changed` | remove `build/qemu-jit` and `build/qemu-jit.qemu-source`, rerun step 2 |
| `Linux did not reach its userspace m5 exit` | `--initrd` missing — `--switch-to-o3` switches on the marker `/init` executes |
| `RISC-V cross compiler not found` / `OpenSBI firmware not found` | step 0's `gcc-riscv64-linux-gnu` / `opensbi` packages |
| scons wedges after hand-deleting a build artifact | `rm build/RISCV/gem5.build/sconsign.dblite`, rebuild |

Guest-visible behaviour is deterministic, so a run that worked in rehearsal works
on stage; the failures above are all environment, not simulation.
