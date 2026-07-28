#!/bin/sh
# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
#
# Build the RISC-V Linux artifacts the JitCPU full-system regressions need.
#
# Produces, in the build directory (default build/jitcpu-linux):
#
#   vmlinux                   kernel ELF; pass as --kernel / --linux-kernel
#   fw_jump.elf               OpenSBI bootloader; pass as the positional image
#   bin/riscv64-linux-musl-gcc
#                             ISA-pinned musl compiler for the pthread
#                             dining-philosophers workload; pass as --musl-cc
#
# The regressions supply their own initramfs, so no busybox userspace is
# built here.
#
# ISA notes: JitCPU executes RV64 IMAFDC plus Zicsr, Zifencei, Zba, Zbb and
# Zbs, and gem5's HiFive device tree advertises exactly "rv64imafdc". The
# Ubuntu RISC-V cross toolchain defaults to a much newer profile, so userspace
# is pinned to rv64gc and the kernel's vector support is configured out. The
# 16-core regressions disassemble the workload and fail on any vector
# instruction that slips through.

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=${1:-"$repo_root/build/jitcpu-linux"}

cross=${CROSS_COMPILE:-riscv64-linux-gnu-}
march=${MARCH:-rv64gc}
mabi=${MABI:-lp64d}
kernel_version=${KERNEL_VERSION:-6.12}
musl_version=${MUSL_VERSION:-1.2.5}
jobs=${JOBS:-$(nproc 2>/dev/null || echo 4)}
opensbi=${OPENSBI_FW_JUMP:-/usr/lib/riscv64-linux-gnu/opensbi/generic/fw_jump.elf}

mkdir -p "$build_dir"
build_dir=$(CDPATH= cd -- "$build_dir" && pwd)
src_dir="$build_dir/src"
musl_prefix="$build_dir/musl"
mkdir -p "$src_dir" "$build_dir/bin"

log() { printf '[build-linux-image] %s\n' "$*"; }

if ! command -v "${cross}gcc" >/dev/null 2>&1; then
    printf '%s\n' "RISC-V cross compiler ${cross}gcc not found" >&2
    printf '%s\n' "On Ubuntu: sudo apt install gcc-riscv64-linux-gnu" >&2
    exit 1
fi
if [ ! -f "$opensbi" ]; then
    printf '%s\n' "OpenSBI firmware not found at $opensbi" >&2
    printf '%s\n' "On Ubuntu: sudo apt install opensbi" >&2
    printf '%s\n' "Or set OPENSBI_FW_JUMP to an fw_jump.elf" >&2
    exit 1
fi

fetch() {
    [ -f "$2" ] && return 0
    curl -L --fail --retry 5 --retry-connrefused --connect-timeout 30 \
        -o "$2.part" "$1"
    mv "$2.part" "$2"
}

log "Downloading sources into $src_dir"
fetch "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${kernel_version}.tar.xz" \
    "$src_dir/linux-${kernel_version}.tar.xz"
fetch "https://musl.libc.org/releases/musl-${musl_version}.tar.gz" \
    "$src_dir/musl-${musl_version}.tar.gz"

[ -d "$src_dir/linux-${kernel_version}" ] ||
    tar -C "$src_dir" -xf "$src_dir/linux-${kernel_version}.tar.xz"
[ -d "$src_dir/musl-${musl_version}" ] ||
    tar -C "$src_dir" -xf "$src_dir/musl-${musl_version}.tar.gz"

linux_src="$src_dir/linux-${kernel_version}"
# Everything the kernel Makefile generates goes here. Keeping the unpacked
# source tree pristine is what lets headers_install and the kernel build
# share one checkout: an in-tree artifact makes every later O= build refuse
# to start with "The source tree is not clean".
kbuild="$build_dir/linux-build"
mkdir -p "$kbuild"

# -------------------------------------------------------------------------
# musl, plus the Linux UAPI headers it does not ship
# -------------------------------------------------------------------------
if [ ! -x "$musl_prefix/bin/musl-gcc" ]; then
    log "Building musl ${musl_version} for $march/$mabi"
    (
        cd "$src_dir/musl-${musl_version}"
        make distclean >/dev/null 2>&1 || true
        ./configure --prefix="$musl_prefix" --syslibdir="$musl_prefix/lib" \
            --target=riscv64-linux-gnu \
            CC="${cross}gcc" CFLAGS="-march=$march -mabi=$mabi -O2" >/dev/null
        make -j"$jobs" >/dev/null
        make install >/dev/null
    )
    make -C "$linux_src" O="$kbuild" ARCH=riscv \
        INSTALL_HDR_PATH="$musl_prefix" headers_install >/dev/null
fi

# musl-gcc honours the toolchain's default ISA, which on Ubuntu is far newer
# than what JitCPU decodes. Pin it here so every caller gets rv64gc.
cat > "$build_dir/bin/riscv64-linux-musl-gcc" <<EOF
#!/bin/sh
exec "$musl_prefix/bin/musl-gcc" -march=$march -mabi=$mabi "\$@"
EOF
chmod +x "$build_dir/bin/riscv64-linux-musl-gcc"

# -------------------------------------------------------------------------
# Kernel
# -------------------------------------------------------------------------
if [ ! -f "$build_dir/vmlinux" ]; then
    log "Configuring Linux ${kernel_version}"
    make -C "$linux_src" O="$kbuild" ARCH=riscv CROSS_COMPILE="$cross" \
        defconfig >/dev/null
    # NR_CPUS must cover the 16-hart mesh; the rest is the console, initramfs
    # and devtmpfs the regressions rely on. Vector is configured out because
    # JitCPU is built with enable_rvv=False.
    "$linux_src/scripts/config" --file "$kbuild/.config" \
        --enable SMP --set-val NR_CPUS 64 \
        --enable BLK_DEV_INITRD \
        --enable SERIAL_8250 --enable SERIAL_8250_CONSOLE \
        --enable SERIAL_OF_PLATFORM \
        --enable SERIAL_EARLYCON --enable SERIAL_EARLYCON_RISCV_SBI \
        --enable DEVTMPFS --enable DEVTMPFS_MOUNT \
        --enable PRINTK --enable TTY --enable RISCV_SBI --enable MMU \
        --disable RISCV_ISA_V
    make -C "$linux_src" O="$kbuild" ARCH=riscv CROSS_COMPILE="$cross" \
        olddefconfig >/dev/null

    log "Building vmlinux with -j$jobs (this is the long step)"
    make -C "$linux_src" O="$kbuild" ARCH=riscv CROSS_COMPILE="$cross" \
        -j"$jobs" vmlinux >/dev/null
    cp "$kbuild/vmlinux" "$build_dir/vmlinux"
else
    log "Kernel already built, skipping"
fi

cp "$opensbi" "$build_dir/fw_jump.elf"

log "Artifacts:"
ls -l "$build_dir/vmlinux" "$build_dir/fw_jump.elf" \
    "$build_dir/bin/riscv64-linux-musl-gcc"
