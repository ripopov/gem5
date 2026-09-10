#!/bin/sh
# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
#
# Build a Linux kernel and initramfs for RV64 MSU, without KVM or H.
# The guest marks the end of boot with m5 workbegin, then checks RAM,
# exercises a timer, lists the root directory and exits with m5 exit.
#
#   util/riscv-bench/build-linux.sh [output-directory]
#
# Environment:
#   LINUX_SRC     Linux source tree (validated with 6.12)
#   BOOTLOADER    OpenSBI fw_jump.elf, jumping to 0x80200000
#   BUSYBOX       static RISC-V BusyBox executable
#   M5            optional static m5 utility (built from util/m5 by default)
#   CROSS_COMPILE compiler prefix (default: riscv64-linux-gnu-)
#   JOBS          make parallelism (default: number of host CPUs)
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
outdir=${1:-"$repo_root/build/riscv-bench/linux"}
linux_src=${LINUX_SRC:?Set LINUX_SRC to a Linux source tree}
bootloader=${BOOTLOADER:?Set BOOTLOADER to OpenSBI fw_jump.elf}
busybox=${BUSYBOX:?Set BUSYBOX to a static RISC-V BusyBox}
m5=${M5:-"$repo_root/util/m5/build/riscv/out/m5"}
if [ -z "${M5:-}" ]; then
    scons -C "$repo_root/util/m5" \
        "riscv.CROSS_COMPILE=${CROSS_COMPILE:-riscv64-linux-gnu-}" \
        build/riscv/out/m5
fi
for f in "$busybox" "$m5"; do
    [ -x "$f" ] || { echo "missing $f: a static RISC-V build is needed" >&2; exit 1; }
done
[ -f "$bootloader" ] || { echo "missing $bootloader" >&2; exit 1; }
mkdir -p "$outdir"
outdir=$(CDPATH= cd -- "$outdir" && pwd)
out="$outdir/initramfs.cpio"

# Keep the kernel release string reproducible across invocations.
export KBUILD_BUILD_TIMESTAMP="1970-01-01 00:00:00 +0000"
export KBUILD_BUILD_USER=gem5
export KBUILD_BUILD_HOST=gem5
export KBUILD_BUILD_VERSION=1
kernel_make() {
    make -C "$linux_src" O="$outdir/kernel" ARCH=riscv \
        CROSS_COMPILE="${CROSS_COMPILE:-riscv64-linux-gnu-}" "$@"
}
kernel_make defconfig
"$linux_src/scripts/config" --file "$outdir/kernel/.config" \
    --disable VIRTUALIZATION --disable KVM \
    --enable RISCV_ISA_V --enable BLK_DEV_INITRD \
    --enable DEVTMPFS --enable DEVTMPFS_MOUNT \
    --enable SERIAL_8250 --enable SERIAL_8250_CONSOLE
kernel_make olddefconfig
if grep -Eq '^CONFIG_(KVM|VIRTUALIZATION)=y' "$outdir/kernel/.config"; then
    echo "The benchmark kernel must not enable virtualization" >&2
    exit 1
fi
kernel_make -j "${JOBS:-$(nproc)}" vmlinux
cp "$outdir/kernel/vmlinux" "$outdir/vmlinux"
cp "$outdir/kernel/.config" "$outdir/kernel.config"
cp "$bootloader" "$outdir/fw_jump.elf"

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/bin" "$stage/sbin" "$stage/dev" "$stage/proc" \
    "$stage/sys" "$stage/tmp" "$stage/root" "$stage/etc"
cp "$busybox" "$stage/bin/busybox"
cp "$m5" "$stage/sbin/m5"
for applet in sh ls mount echo uname cat; do ln -s busybox "$stage/bin/$applet"; done
cat > "$stage/init" <<'INIT'
#!/bin/sh
set -e
export PATH=/sbin:/bin
mount -t devtmpfs devtmpfs /dev 2>/dev/null || true
mount -t proc proc /proc
mount -t sysfs sysfs /sys
echo "RISCV-BENCH: userspace up"
echo "RAM survives CPU takeover" > /tmp/handoff
m5 --inst workbegin 0 0
if [ "$(cat /tmp/handoff)" != "RAM survives CPU takeover" ]; then
    echo "RISCV-BENCH: memory mismatch"
    m5 --inst fail 1
fi
/bin/busybox dd if=/dev/zero of=/tmp/zeros bs=4096 count=64
/bin/busybox sleep 0.01
echo "RISCV-BENCH: memory and timer passed"
uname -a
ls -la /
cat /proc/cpuinfo
echo "RISCV-BENCH: done"
m5 --inst exit
INIT
chmod 755 "$stage/init"

# Write the archive with a small newc encoder rather than cpio(1) so the
# /dev/console character device can be included without root privileges;
# the kernel opens the console before /init runs only if the node exists.
python3 - "$stage" "$out" <<'PY'
import os, stat, sys
stage, out = sys.argv[1], sys.argv[2]
ino = 721

def header(name, mode, nlink, filesize, rdev=(0, 0)):
    global ino
    ino += 1
    name = name.encode() + b"\0"
    fields = [ino, mode, 0, 0, nlink, 0, filesize, 3, 1, *rdev, len(name), 0]
    return (b"070701" + b"".join(f"{f:08x}".encode() for f in fields)
            + name).ljust(-(-(110 + len(name)) // 4) * 4, b"\0")

def pad(data):
    return data.ljust(-(-len(data) // 4) * 4, b"\0")

entries = []
for dirpath, dirnames, filenames in os.walk(stage):
    dirnames.sort()
    for name in sorted(dirnames + filenames):
        full = os.path.join(dirpath, name)
        rel = os.path.relpath(full, stage)
        st = os.lstat(full)
        if stat.S_ISDIR(st.st_mode):
            entries.append(header(rel, st.st_mode & 0o7777 | stat.S_IFDIR, 2, 0))
        elif stat.S_ISLNK(st.st_mode):
            target = os.readlink(full).encode()
            entries.append(header(rel, st.st_mode, 1, len(target)) + pad(target))
        else:
            data = open(full, "rb").read()
            entries.append(header(rel, st.st_mode, 1, len(data)) + pad(data))
entries.append(header("dev/console", stat.S_IFCHR | 0o600, 1, 0, (5, 1)))
entries.append(header("TRAILER!!!", 0, 1, 0))
with open(out, "wb") as f:
    f.write(pad(b"".join(entries)))
PY
echo "Benchmark initramfs: $out"
