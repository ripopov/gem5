#!/bin/sh
# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
#
# Assemble the initramfs for the Linux boot benchmark: a static BusyBox, the
# m5 guest utility, and an /init that lists the root directory and exits
# the simulation through m5_exit. This mirrors Spike's "boot + ls" workload
# with gem5's own exit mechanism in place of SBI poweroff.
#
#   util/riscv-bench/build-linux-initramfs.sh [output.cpio]
#
# Environment:
#   BUSYBOX   static RISC-V BusyBox (default: build/riscv-bench/busybox)
#   M5        static RISC-V m5 utility (default: build/riscv-bench/m5)
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
out=${1:-"$repo_root/build/riscv-bench/bench-initramfs.cpio"}
busybox=${BUSYBOX:-"$repo_root/build/riscv-bench/busybox"}
m5=${M5:-"$repo_root/build/riscv-bench/m5"}
for f in "$busybox" "$m5"; do
    [ -x "$f" ] || { echo "missing $f: a static RISC-V build is needed" >&2; exit 1; }
done

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/bin" "$stage/sbin" "$stage/dev" "$stage/proc" \
    "$stage/sys" "$stage/tmp" "$stage/root" "$stage/etc"
cp "$busybox" "$stage/bin/busybox"
cp "$m5" "$stage/sbin/m5"
for applet in sh ls mount echo uname cat; do ln -s busybox "$stage/bin/$applet"; done
cat > "$stage/init" <<'INIT'
#!/bin/sh
export PATH=/sbin:/bin
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mount -t proc proc /proc
mount -t sysfs sysfs /sys
echo "RISCV-BENCH: userspace up"
uname -a
ls -la /
cat /proc/cpuinfo
echo "RISCV-BENCH: done"
m5 exit
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
