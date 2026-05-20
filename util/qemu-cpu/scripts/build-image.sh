#!/usr/bin/env bash
#
# build-image.sh - Build a minimal RISC-V Linux image for gem5 QEMU-CPU mode.
#
# Produces, in $IMG (default: <repo>/images):
#   - Image              : raw RISC-V kernel image (for QEMU -kernel / gem5)
#   - vmlinux            : kernel ELF (with symbols, used by gem5 loader)
#   - initramfs.cpio.gz  : busybox root filesystem (boots to a shell)
#   - fw_jump.bin        : OpenSBI firmware (copied from the host packages)
#
# ISA notes
# ---------
# The Ubuntu RISC-V cross toolchain targets the RVA23 profile (vector, vector
# crypto, Zicond, ...), which gem5 does not fully decode.  To keep every
# executed instruction inside gem5's supported set we:
#   * build userspace (musl + busybox) for plain "rv64gc" - no vector, no
#     Zb*, no Zicond;
#   * keep the kernel (its C code is compiled "rv64imac" by the kernel
#     Makefile regardless of the toolchain default) and constrain the QEMU
#     CPU at boot so the kernel's runtime "alternatives" never patch in
#     instructions gem5 lacks (see qemu-common.sh: QEMU_CPU).
#
# An initramfs (no disk) is used so a QEMU snapshot contains no virtio
# block-device state - the whole filesystem is just guest RAM.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
IMG="${IMG:-$REPO_ROOT/images}"
SRC="$IMG/src"
JOBS="$(nproc)"

CROSS="${CROSS:-riscv64-linux-gnu-}"
MARCH="${MARCH:-rv64gc}"
MABI="${MABI:-lp64d}"
KERNEL_VER="${KERNEL_VER:-6.12}"
BUSYBOX_VER="${BUSYBOX_VER:-1.36.1}"
MUSL_VER="${MUSL_VER:-1.2.5}"

mkdir -p "$SRC"
cd "$SRC"
log() { echo -e "\n\033[1;36m[build-image] $*\033[0m"; }

# --------------------------------------------------------------------------
# 1. Download sources
# --------------------------------------------------------------------------
log "Downloading sources (if missing)"
fetch() { [ -f "$2" ] || curl -L --fail -o "$2" "$1"; }
fetch "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${KERNEL_VER}.tar.xz" "linux-${KERNEL_VER}.tar.xz"
fetch "https://busybox.net/downloads/busybox-${BUSYBOX_VER}.tar.bz2"            "busybox-${BUSYBOX_VER}.tar.bz2"
fetch "https://musl.libc.org/releases/musl-${MUSL_VER}.tar.gz"                  "musl-${MUSL_VER}.tar.gz"

[ -d "linux-${KERNEL_VER}" ]   || tar xf "linux-${KERNEL_VER}.tar.xz"
[ -d "busybox-${BUSYBOX_VER}" ] || tar xf "busybox-${BUSYBOX_VER}.tar.bz2"
[ -d "musl-${MUSL_VER}" ]       || tar xf "musl-${MUSL_VER}.tar.gz"

# --------------------------------------------------------------------------
# 2. Build musl libc (rv64gc, scalar)
# --------------------------------------------------------------------------
MUSL="$SRC/musl"
if [ ! -x "$MUSL/bin/musl-gcc" ]; then
    log "Building musl ${MUSL_VER} ($MARCH/$MABI)"
    cd "$SRC/musl-${MUSL_VER}"
    make distclean >/dev/null 2>&1 || true
    ./configure --prefix="$MUSL" --syslibdir="$MUSL/lib" \
        --target=riscv64-linux-gnu \
        CC="${CROSS}gcc" CFLAGS="-march=$MARCH -mabi=$MABI -O2" >/dev/null
    make -j"$JOBS" >/dev/null
    make install >/dev/null
    # busybox needs the Linux UAPI headers (linux/*.h, asm/*.h); musl does
    # not ship them - install them from the kernel tree next to musl's.
    log "Installing Linux UAPI headers into musl"
    make -C "$SRC/linux-${KERNEL_VER}" ARCH=riscv \
        INSTALL_HDR_PATH="$MUSL" headers_install >/dev/null
fi
# A compiler wrapper that pins the ISA so neither busybox nor musl pull in
# vector / Zb* / Zicond code paths.
MUSLCC="$MUSL/bin/musl-gcc -march=$MARCH -mabi=$MABI"

# --------------------------------------------------------------------------
# 3. Build busybox (static, against musl) and assemble the initramfs
# --------------------------------------------------------------------------
log "Building busybox ${BUSYBOX_VER} (static musl, $MARCH)"
cd "$SRC/busybox-${BUSYBOX_VER}"
make ARCH=riscv CROSS_COMPILE="$CROSS" defconfig >/dev/null
sed -i 's/.*CONFIG_STATIC[ =].*/CONFIG_STATIC=y/'  .config
sed -i 's/.*CONFIG_TC[ =].*/CONFIG_TC=n/'          .config
make ARCH=riscv CROSS_COMPILE="$CROSS" oldconfig </dev/null >/dev/null
make ARCH=riscv CROSS_COMPILE="$CROSS" CC="$MUSLCC" -j"$JOBS" >/dev/null
rm -rf "$SRC/rootfs"
make ARCH=riscv CROSS_COMPILE="$CROSS" CC="$MUSLCC" \
     CONFIG_PREFIX="$SRC/rootfs" install >/dev/null

log "Assembling initramfs"
ROOTFS="$SRC/rootfs"
mkdir -p "$ROOTFS"/{proc,sys,dev,tmp,root,etc}

cat > "$ROOTFS/init" <<'INIT'
#!/bin/sh
# Minimal init for gem5 QEMU-CPU mode bring-up.
/bin/busybox --install -s /bin 2>/dev/null
mount -t proc     proc     /proc
mount -t sysfs    sysfs    /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null
export PS1='qemucpu# '
# Marker the snapshot driver waits for, then drop to an idle shell.
echo
echo "QEMU-CPU-MODE-SHELL-READY"
exec /bin/sh
INIT
chmod +x "$ROOTFS/init"

( cd "$ROOTFS" && find . -print0 \
    | cpio --null -ov --format=newc 2>/dev/null \
    | gzip -9 > "$IMG/initramfs.cpio.gz" )
log "initramfs -> $IMG/initramfs.cpio.gz ($(du -h "$IMG/initramfs.cpio.gz" | cut -f1))"

# Quick sanity check: the userspace must be free of vector instructions.
VCOUNT=$("${CROSS}objdump" -d "$ROOTFS/bin/busybox" \
            | grep -cE '\t(v[a-z]+[0-9]?\.|vset)' || true)
echo "[build-image] vector instructions in busybox: $VCOUNT (expected 0)"

# --------------------------------------------------------------------------
# 4. Build the kernel (skip if already present)
# --------------------------------------------------------------------------
if [ ! -f "$IMG/Image" ]; then
    log "Configuring kernel ${KERNEL_VER}"
    cd "$SRC/linux-${KERNEL_VER}"
    make ARCH=riscv CROSS_COMPILE="$CROSS" defconfig >/dev/null
    ./scripts/config \
        --enable BLK_DEV_INITRD \
        --enable SERIAL_8250 --enable SERIAL_8250_CONSOLE \
        --enable SERIAL_OF_PLATFORM \
        --enable SERIAL_EARLYCON --enable SERIAL_EARLYCON_RISCV_SBI \
        --enable DEVTMPFS --enable DEVTMPFS_MOUNT \
        --enable PRINTK --enable TTY --enable RISCV_SBI --enable MMU
    make ARCH=riscv CROSS_COMPILE="$CROSS" olddefconfig >/dev/null

    log "Building kernel (-j$JOBS) - this is the long step"
    make ARCH=riscv CROSS_COMPILE="$CROSS" -j"$JOBS" Image vmlinux >/dev/null
    cp arch/riscv/boot/Image "$IMG/Image"
    cp vmlinux               "$IMG/vmlinux"
else
    log "Kernel already built ($IMG/Image) - skipping"
fi
log "kernel -> $IMG/Image ($(du -h "$IMG/Image" | cut -f1))"

# --------------------------------------------------------------------------
# 5. OpenSBI firmware (from the host packages)
# --------------------------------------------------------------------------
log "Copying OpenSBI firmware"
cp /usr/lib/riscv64-linux-gnu/opensbi/generic/fw_jump.bin "$IMG/fw_jump.bin"

log "DONE. Artifacts in $IMG:"
ls -lh "$IMG"/Image "$IMG"/vmlinux "$IMG"/initramfs.cpio.gz "$IMG"/fw_jump.bin
