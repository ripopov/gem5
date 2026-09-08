#!/bin/bash
# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
#
# Run a gem5 RISC-V functional-CPU workload N times and report throughput.
#
#   util/riscv-bench/bench.sh coremark [N] [gem5 args...]
#   util/riscv-bench/bench.sh linux    [N] [gem5 args...]
#
# Environment:
#   GEM5        simulator binary (default build/RISCV/gem5.fast)
#   OUTDIR      gem5 output directory (default build/riscv-bench/m5out-<wl>)
#   COREMARK    bare-metal image (default build/riscv-bench/coremark.elf)
#   BOOTLOADER, KERNEL, INITRD   Linux boot artifacts (defaults under
#                                build/riscv-bench/linux/)
#   TASKSET     CPU list to pin to (default 2)
#
# Instructions come from gem5's simInsts and host time from hostSeconds, so
# the MIPS figure excludes Python start-up and image loading; the wall-clock
# time of the whole process is printed alongside for comparison with Spike.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
workload=${1:?usage: bench.sh coremark|linux [N] [gem5 args...]}
runs=${2:-3}
shift $(( $# >= 2 ? 2 : 1 ))
gem5=${GEM5:-"$repo_root/build/RISCV/gem5.fast"}
outdir=${OUTDIR:-"$repo_root/build/riscv-bench/m5out-$workload"}
config="$repo_root/configs/example/riscv/noncaching_fs.py"
pin=${TASKSET:-2}

case "$workload" in
coremark)
    image=${COREMARK:-"$repo_root/build/riscv-bench/coremark.elf"}
    set -- baremetal "$image" "$@"
    check() {
        for crc in 'crclist       : 0xe714' 'crcmatrix     : 0x1fd7' \
                   'crcstate      : 0x8e3a'; do
            grep -q "$crc" "$outdir/system.platform.terminal" || {
                echo "run $i FAILED: missing '$crc'" >&2
                cat "$outdir/system.platform.terminal" >&2
                exit 1
            }
        done
    }
    ;;
linux)
    set -- linux \
        --bootloader "${BOOTLOADER:-$repo_root/build/riscv-bench/linux/fw_jump.elf}" \
        --kernel "${KERNEL:-$repo_root/build/riscv-bench/linux/vmlinux}" \
        --initrd "${INITRD:-$repo_root/build/riscv-bench/bench-initramfs.cpio}" \
        "$@"
    check() {
        grep -q "RISCV-BENCH: done" "$outdir/system.platform.terminal" || {
            echo "run $i FAILED: guest did not reach the end marker" >&2
            tail -20 "$outdir/system.platform.terminal" >&2
            exit 1
        }
    }
    ;;
*) echo "unknown workload $workload" >&2; exit 1 ;;
esac

for i in $(seq 1 "$runs"); do
    start=$(date +%s.%N)
    taskset -c "$pin" "$gem5" --outdir="$outdir" "$config" "$@" \
        > "$outdir.log" 2>&1 || { tail -20 "$outdir.log" >&2; exit 1; }
    end=$(date +%s.%N)
    check
    insts=$(awk '/^simInsts/{print $2}' "$outdir/stats.txt")
    host=$(awk '/^hostSeconds/{print $2}' "$outdir/stats.txt")
    echo "$host $insts $(echo "$end - $start" | bc -l)"
done | awk -v gem5="$gem5" -v wl="$workload" '
  { t=$1; ins=$2; s+=t; p+=$3; if (NR==1 || t<min) min=t; if (NR==1||$3<pmin) pmin=$3 }
  END {
    mean=s/NR
    printf "binary          : %s\n", gem5
    printf "workload        : %s\n", wl
    printf "runs            : %d\n", NR
    printf "instructions    : %d\n", ins
    printf "hostSeconds mean: %.3f s\n", mean
    printf "hostSeconds best: %.3f s\n", min
    printf "process mean    : %.3f s\n", p/NR
    printf "process best    : %.3f s\n", pmin
    printf "MIPS mean       : %.2f  (hostSeconds)\n", ins/mean/1e6
    printf "MIPS best       : %.2f  (hostSeconds)\n", ins/min/1e6
    printf "MIPS process    : %.2f  (whole process, mean)\n", ins/(p/NR)/1e6
  }'
