#!/usr/bin/env bash
#
# qemu-boot.sh - boot the minimal RISC-V image under QEMU, interactively.
#
# Handy for manually checking the image before/after changing it.  The VM
# uses the exact same machine/CPU/memory configuration as the snapshot
# driver (qemu-snapshot.py).  Exit with Ctrl-A x.
#
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/qemu-common.sh"

exec "$QEMU_BIN" \
    -machine virt \
    -cpu "$QEMU_CPU" \
    -smp "${QEMU_SMP:-1}" -m "${QEMU_MEM_MB}M" \
    -bios "$BIOS" \
    -kernel "$KERNEL" \
    -initrd "$INITRD" \
    -append "$KCMDLINE qemucpu.mode=shell" \
    -nographic -no-reboot "$@"
