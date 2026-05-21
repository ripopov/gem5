#!/usr/bin/env bash
#
# qemu-common.sh - shared settings for gem5 QEMU-snapshot mode scripts.
#
# Source this from other scripts:  . "$(dirname "$0")/qemu-common.sh"
#
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
IMG="${IMG:-$REPO_ROOT/images}"

# Guest RAM: base is fixed by the QEMU 'virt' machine; size is our choice.
QEMU_MEM_MB="${QEMU_MEM_MB:-256}"
GUEST_RAM_BASE="0x80000000"

# CPU model.  Start from QEMU's feature-rich rv64 and switch off every
# extension gem5's RISC-V decoder does not implement, so the kernel's boot
# time "alternatives" patching never inserts an instruction gem5 cannot run.
# Also pin paging to Sv39 (sv48/sv57 off) which gem5 supports.
QEMU_CPU="${QEMU_CPU:-rv64,v=false,h=false,sstc=false,zicond=false,zacas=false,zawrs=false,zbc=false,zbkb=false,zfa=false,zfh=false,zfhmin=false,svadu=false,sv57=false,sv48=false}"

QEMU_BIN="${QEMU_BIN:-qemu-system-riscv64}"
KERNEL="${KERNEL:-$IMG/Image}"
INITRD="${INITRD:-$IMG/initramfs.cpio.gz}"
BIOS="${BIOS:-$IMG/fw_jump.bin}"
KCMDLINE="${KCMDLINE:-console=ttyS0 earlycon=sbi}"

# Marker printed by the initramfs /init once the shell is ready.
SHELL_MARKER="QEMU-SNAP-MODE-SHELL-READY"
