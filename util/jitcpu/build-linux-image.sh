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
#   busybox-initramfs.cpio     interactive BusyBox userspace; pass as --initrd
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
busybox_version=${BUSYBOX_VERSION:-1.36.1}
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
fetch "https://busybox.net/downloads/busybox-${busybox_version}.tar.bz2" \
    "$src_dir/busybox-${busybox_version}.tar.bz2"

[ -d "$src_dir/linux-${kernel_version}" ] ||
    tar -C "$src_dir" -xf "$src_dir/linux-${kernel_version}.tar.xz"
[ -d "$src_dir/musl-${musl_version}" ] ||
    tar -C "$src_dir" -xf "$src_dir/musl-${musl_version}.tar.gz"
[ -d "$src_dir/busybox-${busybox_version}" ] ||
    tar -C "$src_dir" -xf "$src_dir/busybox-${busybox_version}.tar.bz2"

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
wrapper_dir=\$(CDPATH= cd -- "\$(dirname -- "\$0")" && pwd)
exec "\$wrapper_dir/../musl/bin/musl-gcc" \
    -march=$march -mabi=$mabi "\$@"
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
    # JitCPU's scalar ISA profile does not report the V extension.
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

# -------------------------------------------------------------------------
# Static BusyBox interactive initramfs
# -------------------------------------------------------------------------
busybox_src="$src_dir/busybox-${busybox_version}"
busybox_build="$build_dir/busybox-build"
busybox_rootfs="$build_dir/busybox-rootfs"
busybox_initramfs="$build_dir/busybox-initramfs.cpio"

if [ ! -x "$busybox_build/busybox" ]; then
    log "Building static BusyBox ${busybox_version} for $march/$mabi"
    mkdir -p "$busybox_build"
    make -C "$busybox_src" O="$busybox_build" ARCH=riscv \
        CROSS_COMPILE="$cross" defconfig >/dev/null
    sed -i \
        -e 's/^# CONFIG_STATIC is not set$/CONFIG_STATIC=y/' \
        -e 's/^CONFIG_TC=y$/# CONFIG_TC is not set/' \
        "$busybox_build/.config"
    make -C "$busybox_src" O="$busybox_build" ARCH=riscv \
        CROSS_COMPILE="$cross" \
        CFLAGS_BUSYBOX="-march=$march -mabi=$mabi" \
        -j"$jobs" >/dev/null
else
    log "BusyBox already built, repacking the initramfs"
fi

rm -rf "$busybox_rootfs"
mkdir -p "$busybox_rootfs"
make -C "$busybox_src" O="$busybox_build" ARCH=riscv \
    CROSS_COMPILE="$cross" CONFIG_PREFIX="$busybox_rootfs" \
    install >/dev/null
mkdir -p "$busybox_rootfs/dev/pts" "$busybox_rootfs/etc" \
    "$busybox_rootfs/proc" "$busybox_rootfs/root" \
    "$busybox_rootfs/sys" "$busybox_rootfs/tmp"
chmod 1777 "$busybox_rootfs/tmp"
cat > "$busybox_rootfs/init" <<'EOF'
#!/bin/sh
export HOME=/root
export PATH=/sbin:/bin:/usr/sbin:/usr/bin

mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts 2>/dev/null || true
mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
hostname jitcpu

echo
echo "JITCPU-BUSYBOX READY"
echo "Interactive RISC-V shell on ttyS0; power off with: poweroff -f"
exec setsid cttyhack /bin/sh
EOF
chmod 755 "$busybox_rootfs/init"
(
    cd "$busybox_rootfs"
    find . -print | LC_ALL=C sort | cpio --quiet -o -H newc \
        > "$busybox_initramfs"
)

cp "$opensbi" "$build_dir/fw_jump.elf"

log "Artifacts:"
ls -l "$build_dir/vmlinux" "$build_dir/fw_jump.elf" \
    "$build_dir/busybox-initramfs.cpio" \
    "$build_dir/bin/riscv64-linux-musl-gcc"
