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

Rerun `build-qemu-jit.sh` after every change under `ext/qemu/gem5-jit`. The
configs take the backend as a path and load it with `dlopen`, so a shared
library left over from an older adapter is used without complaint, and the
resulting failures look like CPU-model bugs rather than a stale build.

The backend build also compiles and runs `gem5-qemu-jit-smoke`, a two-hart
adapter unit test covering independent per-hart state, execution, and
translation invalidation.

## 2. Build the Linux artifacts

The full-system runs need a kernel ELF, an OpenSBI bootloader and a musl
compiler for the pthread dining-philosophers workload. `build-linux-image.sh`
produces all three:

```sh
sudo apt install gcc-riscv64-linux-gnu opensbi \
    bc bison flex libelf-dev libssl-dev

util/jitcpu/build-linux-image.sh       # -> build/jitcpu-linux/
```

It downloads Linux 6.12 and musl 1.2.5, then writes:

| Artifact | Passed as |
| --- | --- |
| `build/jitcpu-linux/vmlinux` | `--kernel` |
| `build/jitcpu-linux/fw_jump.elf` | the positional Linux image |
| `build/jitcpu-linux/bin/riscv64-linux-musl-gcc` | `DINING_CC` for the payload Makefile |

The kernel is `defconfig` plus `NR_CPUS=64`, an initramfs, the 8250 console
and devtmpfs, with `RISCV_ISA_V` configured out. Userspace is pinned to
`rv64gc`, because gem5's device tree advertises `rv64imafdc` and JitCPU
decodes nothing beyond that plus Zicsr/Zifencei/Zba/Zbb/Zbs. Set
`KERNEL_VERSION`, `MUSL_VERSION`, `MARCH` or `JOBS` to override. Building the
kernel is the long step; rerunning the script reuses it.

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
