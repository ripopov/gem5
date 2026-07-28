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
drivers take the backend as a path and load it with `dlopen`, so a shared
library left over from an older adapter is used without complaint, and the
resulting failures look like CPU-model bugs rather than a stale build.

## 2. Bare-metal and single/four-hart regressions

These need only the cross toolchain, and cover 21 alternating JitCPU/O3
switches on one, four and eight harts plus the 16-hart bare-metal mesh:

```sh
sudo apt install gcc-riscv64-linux-gnu

tests/gem5/jitcpu/run_repeated_switch_regression.py \
    build/RISCV/gem5.opt build/qemu-jit/libgem5-qemu-jit.so
```

Pass `--cross-compile` if your toolchain prefix is not `riscv64-linux-gnu-`.

## 3. Build the Linux artifacts

The full-system phases need a kernel ELF, an OpenSBI bootloader and a musl
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
| `build/jitcpu-linux/vmlinux` | `--kernel` / `--linux-kernel` |
| `build/jitcpu-linux/fw_jump.elf` | the positional Linux image |
| `build/jitcpu-linux/bin/riscv64-linux-musl-gcc` | `--musl-cc` |

The kernel is `defconfig` plus `NR_CPUS=64`, an initramfs, the 8250 console
and devtmpfs, with `RISCV_ISA_V` configured out. Userspace is pinned to
`rv64gc`, because gem5's device tree advertises `rv64imafdc` and JitCPU
decodes nothing beyond that plus Zicsr/Zifencei/Zba/Zbb/Zbs; the 16-core
driver disassembles the workload and fails on any vector instruction. Set
`KERNEL_VERSION`, `MUSL_VERSION`, `MARCH` or `JOBS` to override. Building the
kernel is the long step; rerunning the script reuses it.

## 4. Full-system and 16-hart mesh regressions

`run_repeated_switch_regression.py --linux-image` adds Linux repeated-switch
stress on one and four harts to section 2:

```sh
tests/gem5/jitcpu/run_repeated_switch_regression.py \
    build/RISCV/gem5.opt build/qemu-jit/libgem5-qemu-jit.so \
    --linux-image build/jitcpu-linux/fw_jump.elf \
    --linux-kernel build/jitcpu-linux/vmlinux
```

`run_16core_mesh_regression.py` runs the 16-hart CHI 4x4 mesh suite: the
bare-metal mesh handoff, repeated dining-philosophers runs checked for
tick-identical results, a guest-coordinated multi-switch run, and finally
the whole of section 2:

```sh
tests/gem5/jitcpu/run_16core_mesh_regression.py \
    build/RISCV/gem5.opt build/qemu-jit/libgem5-qemu-jit.so \
    build/jitcpu-linux/fw_jump.elf \
    --kernel build/jitcpu-linux/vmlinux \
    --musl-cc build/jitcpu-linux/bin/riscv64-linux-musl-gcc
```

Add `--skip-existing-regressions` to run only the 16-hart part. If a Linux
phase fails with "no userspace progress", raise `--linux-phase-ticks`: each
bounded O3 phase has to outlast the RTC-interrupt backlog left by the
preceding JitCPU phase, and the amount of catch-up scales with the kernel and
the RTC frequency.

Individual scenarios can also be driven straight from the configs, for
example one 16-hart dining-philosophers handoff:

```sh
build/RISCV/gem5.opt tests/gem5/jitcpu/configs/jitcpu_linux.py \
    build/jitcpu-linux/fw_jump.elf build/qemu-jit/libgem5-qemu-jit.so \
    --kernel build/jitcpu-linux/vmlinux \
    --chi-4x4-mesh --workload-handoff \
    --initrd tests/test-progs/jitcpu-smoke/src/jitcpu-dining-philosophers.cpio \
    --max-ticks 2000000000000 --o3-ticks 50000000000
```

## Configurations

| File | Purpose |
| --- | --- |
| `jitcpu_baremetal.py` | bare-metal JitCPU/O3 switching, classic or CHI |
| `jitcpu_linux.py` | full-system Linux with repeated switches |
| `jitcpu_common.py` | Ruby options and stat helpers both configs share |
| `jitcpu_16core_mesh.py` | 16-hart CHI mesh setup and mesh validation |
| `chi_mesh_4x4.py` | the 4x4 mesh topology those two share |

See `src/cpu/jit/README.md` for what the regressions check and why.
