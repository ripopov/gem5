# JitCPU regressions

These are **not** testlib tests. `RiscvJitCPU` needs a `USE_JITCPU=y` gem5
binary plus the separately built QEMU backend shared library, and the guest
payloads need a RISC-V cross toolchain, so none of it can run unattended in
the normal `tests/gem5` suite. Nothing here matches testlib's `test*.py`
discovery pattern, so `main.py run` ignores this directory.

## 1. Build gem5 and the QEMU backend

`NUMBER_BITS_PER_SET` must cover the 16-hart mesh; the configs refuse to run
otherwise.

```sh
scons build/RISCV/gem5.opt USE_JITCPU=y RUBY_PROTOCOL_CHI=y \
    NUMBER_BITS_PER_SET=128

git submodule update --init --depth 1 ext/qemu/repo
util/jitcpu/build-qemu-jit.sh          # -> build/qemu-jit/libgem5-qemu-jit.so
```

Rerun `build-qemu-jit.sh` after every QEMU submodule update. The configs take
the backend as a path and load it with `dlopen`, so a shared library left over
from an older adapter is used without complaint, and the resulting failures
look like CPU-model bugs rather than a stale build.

The backend build also compiles and runs `gem5-qemu-jit-smoke`, a two-hart
adapter unit test covering independent per-hart state, execution, and
translation invalidation.

## 2. Build the Linux artifacts

The full-system runs need a kernel ELF and an OpenSBI bootloader. The
regressions additionally use a musl compiler for the pthread
dining-philosophers workload, while an interactive run uses the BusyBox
initramfs and a static `m5` utility. `build-linux-image.sh` produces all five
on a Linux host:

```sh
sudo apt install build-essential gcc-riscv64-linux-gnu \
    g++-riscv64-linux-gnu scons \
    libc6-dev-riscv64-cross opensbi bc bison flex \
    libelf-dev libssl-dev cpio curl xz-utils bzip2

util/jitcpu/build-linux-image.sh       # -> build/jitcpu-linux/
```

It downloads Linux 6.12, musl 1.2.5 and BusyBox 1.36.1, then writes:

| Artifact | Passed as |
| --- | --- |
| `build/jitcpu-linux/vmlinux` | `--kernel` |
| `build/jitcpu-linux/fw_jump.elf` | the positional Linux image |
| `build/jitcpu-linux/m5` | installed as `/sbin/m5` in the interactive image |
| `build/jitcpu-linux/busybox-initramfs.cpio` | `--initrd` for an interactive shell |
| `build/jitcpu-linux/bin/riscv64-linux-musl-gcc` | `DINING_CC` for the payload Makefile |

The kernel is `defconfig` plus `NR_CPUS=64`, an initramfs, the 8250 console
and devtmpfs, with `RISCV_ISA_V` configured out. Userspace is pinned to
`rv64gc`, because gem5's device tree advertises `rv64imafdc` and JitCPU
decodes nothing beyond that plus Zicsr/Zifencei/Zba/Zbb/Zbs. Set
`KERNEL_VERSION`, `MUSL_VERSION`, `BUSYBOX_VERSION`, `MARCH` or `JOBS` to
override. Building the kernel is the long step; rerunning the script reuses
it and repacks the initramfs.

On macOS, build the boot artifacts in Docker instead of installing a native
Linux cross-build environment:

```sh
util/jitcpu/build-linux-image-docker.sh # -> build/jitcpu-linux/
```

Linux sources and intermediate files remain in the persistent,
case-sensitive `gem5-jitcpu-linux-build` Docker volume. This matters because
the Linux source tree contains names that collide on the default
case-insensitive macOS filesystem. The wrapper exports `vmlinux`,
`fw_jump.elf`, the static `m5` executable and `busybox-initramfs.cpio` as
ordinary host files. Docker is not used by gem5 and may be stopped after this
command. Set
`JITCPU_LINUX_BUILD_VOLUME` to use a different volume. The musl compiler is
not exported to macOS; build the 16-hart pthread payloads on Linux.

Build the guest payloads:

```sh
make -C tests/test-progs/jitcpu-smoke/src \
    DINING_CC=build/jitcpu-linux/bin/riscv64-linux-musl-gcc
```

## 3. Linux boot and one-way switch runs

Single-hart Linux boot to userspace (the initramfs `init` executes `m5_exit`
as its first userspace instruction), on classic memory and on Ruby CHI:

```sh
build/RISCV/gem5.opt tests/gem5/jitcpu/configs/jitcpu_linux.py \
    build/jitcpu-linux/fw_jump.elf build/qemu-jit/libgem5-qemu-jit.so \
    --kernel build/jitcpu-linux/vmlinux \
    --initrd tests/test-progs/jitcpu-smoke/src/jitcpu-linux-init.cpio

build/RISCV/gem5.opt tests/gem5/jitcpu/configs/jitcpu_linux.py \
    build/jitcpu-linux/fw_jump.elf build/qemu-jit/libgem5-qemu-jit.so \
    --kernel build/jitcpu-linux/vmlinux \
    --initrd tests/test-progs/jitcpu-smoke/src/jitcpu-linux-init.cpio \
    --ruby-chi
```

Boot on JitCPU, then switch the hart to O3 and validate that the O3 phase
makes userspace progress and (with `--ruby-chi`) drives CHI traffic:

```sh
build/RISCV/gem5.opt tests/gem5/jitcpu/configs/jitcpu_linux.py \
    build/jitcpu-linux/fw_jump.elf build/qemu-jit/libgem5-qemu-jit.so \
    --kernel build/jitcpu-linux/vmlinux \
    --initrd tests/test-progs/jitcpu-smoke/src/jitcpu-linux-init.cpio \
    --ruby-chi --switch-to-o3 --o3-ticks 50000000
```

### Interactive serial terminal

`jitcpu_linux.py` attaches gem5's `Terminal` device to the HiFive UART. It
listens on localhost TCP port 3456 by default and prints the actual port if it
has to choose another one. `--terminal-port` changes the requested starting
port. The default `--listener-mode=auto` enables the listener when gem5's
standard input is a terminal; pass `--listener-mode=on` before the
configuration path to enable it explicitly.

For an interactive shell, run gem5 in the first host terminal. On macOS,
replace the backend `.so` below with `.dylib`:

```sh
build/RISCV/gem5.opt --listener-mode=on \
    tests/gem5/jitcpu/configs/jitcpu_linux.py \
    build/jitcpu-linux/fw_jump.elf build/qemu-jit/libgem5-qemu-jit.so \
    --kernel build/jitcpu-linux/vmlinux \
    --initrd build/jitcpu-linux/busybox-initramfs.cpio \
    --interactive-terminal --switch-to-o3 \
    --max-ticks 100000000000000000
```

`--interactive-terminal` pauses after instantiation and reports the actual
bound port. In a second host terminal, connect to that port:

```sh
util/term/gem5term localhost 3456
```

The client initially waits silently. Return to the first terminal and press
Enter to boot. Linux starts `/init`, which mounts `/dev`, `/proc` and `/sys`
and opens a root BusyBox shell on `ttyS0`:

```text
JITCPU-BUSYBOX READY
Interactive RISC-V shell on ttyS0; power off with: poweroff -f
gem5 controls: m5 exit, m5 dumpstats, m5 switchcpu
/ # uname -m
riscv64
/ # m5 dumpstats
```

The image installs the statically linked RISC-V utility as `/sbin/m5`.
`m5 exit` ends the simulation cleanly, while `m5 dumpstats` requests an
immediate statistics dump. The utility also exposes `m5 checkpoint`, but
JitCPU checkpoint/restore is currently broken and is not supported by this
demo.

The command above includes `--switch-to-o3`, which creates the switched-out
O3 CPU needed for the one-way handoff. After the shell appears, run:

```sh
m5 switchcpu
```

The configuration consumes the guest's `switchcpu` exit, calls
`m5.switchCpus()`, and validates that `RiscvO3CPU` makes userspace progress.
The `m5` command then returns to the shell under O3 and the same `gem5term`
connection remains active. Continue using the shell normally and run
`m5 exit` when finished. `--o3-ticks` controls only the initial O3 validation
interval; the interactive session continues until `m5 exit` or
`--max-ticks`. The same option still accepts the checked-in non-interactive
initramfs's historical `m5_exit` handoff marker.

Interactive mode runs gem5 in short slices and synchronously polls the host
terminal between them. This keeps serial input responsive when Linux and
JitCPU are otherwise idle in WFI. After switching, O3 uses shorter simulated
slices so the detailed CPU does not delay terminal polling. Interactive mode
does not require `--classic-rtc-events`.

The listener is restricted to localhost unless gem5 is given
`--allow-remote-connections`. Type `~.` to disconnect the terminal client.
The serial output is also retained in
`m5out/system.platform.terminal` (or the corresponding file under the chosen
gem5 output directory).

The checked-in `jitcpu-linux-init.cpio` remains intentionally non-interactive:
its `/init` executes `m5_exit` at the first userspace instruction and then
runs a memory workload for the O3 validation phase. The generated BusyBox
image waits for an explicit `m5 exit` or `m5 switchcpu` command instead.

## 4. 16-hart CHI mesh boot and workload handoff

The 16-hart dining-philosophers handoff boots Linux on 16 JitCPU harts on
the validated CHI SimpleNetwork 4x4 mesh, runs the pinned pthread workload's
first phase, performs one guest-coordinated whole-system switch to O3, and
validates the second phase (per-worker checksums, affinity, per-RNF and
per-controller traffic) on the timing hierarchy:

```sh
build/RISCV/gem5.opt tests/gem5/jitcpu/configs/jitcpu_linux.py \
    build/jitcpu-linux/fw_jump.elf build/qemu-jit/libgem5-qemu-jit.so \
    --kernel build/jitcpu-linux/vmlinux \
    --chi-4x4-mesh --workload-handoff \
    --initrd tests/test-progs/jitcpu-smoke/src/jitcpu-dining-philosophers.cpio \
    --max-ticks 2000000000000 --o3-ticks 50000000000
```

Every JitCPU phase must show zero CHI messages, zero cache accesses on every
RNF, and zero memory-controller traffic; the O3 phase must show the
opposite. The configs enforce this and fail otherwise.

## Configurations

| File | Purpose |
| --- | --- |
| `jitcpu_linux.py` | full-system Linux boot and one-way JitCPU-to-O3 switch |
| `jitcpu_common.py` | Ruby options and stat helpers |
| `jitcpu_16core_mesh.py` | 16-hart CHI mesh setup and mesh validation |
| `chi_mesh_4x4.py` | the 4x4 mesh topology |

See `src/cpu/jit/README.md` for what the runs check and why.
