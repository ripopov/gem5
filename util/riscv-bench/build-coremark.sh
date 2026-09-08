#!/bin/sh
# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
#
# Build CoreMark as a bare-metal RVA23U64 image for gem5's RISC-V full-system
# platform (configs/example/riscv/noncaching_fs.py baremetal).
#
#   util/riscv-bench/build-coremark.sh [build-dir]
#
# Environment:
#   COREMARK_SRC   upstream eembc/coremark checkout (cloned if missing)
#   ITERATIONS     CoreMark iterations (default 5000, ~1.6 G instructions)
#   MARCH          -march string (default: RVA23U64 mandatory set)
#   CC             cross compiler (default riscv64-unknown-elf-gcc + picolibc)
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=${1:-"$repo_root/build/riscv-bench"}
port_dir="$repo_root/util/riscv-bench/coremark"
coremark_src=${COREMARK_SRC:-"$build_dir/coremark"}
iterations=${ITERATIONS:-5000}
out=${OUT:-"$build_dir/coremark.elf"}
cc=${CC:-riscv64-unknown-elf-gcc}
# RVA23U64 mandatory extensions, spelled out because GCC 14 has no profile
# alias. Zimop and Zcmop are omitted only because GCC 14's bare-metal port
# rejects them in -march; the compiler never emits them anyway.
march=${MARCH:-rv64imafdcv_zicsr_zifencei_zicntr_zihpm_zihintpause_zihintntl_zfhmin_zba_zbb_zbs_zicbom_zicbop_zicboz_zkt_zicond_zcb_zfa_zawrs_zvfhmin_zvbb_zvkt}
mabi=lp64d
# medany: the image lives at 0x80000000, out of reach of medlow's lui/addi.
guest_opt="-O2 -fno-common -mcmodel=medany"

mkdir -p "$build_dir"
if [ ! -f "$coremark_src/core_main.c" ]; then
    git clone --quiet https://github.com/eembc/coremark.git "$coremark_src"
fi
obj="$build_dir/coremark-obj"
mkdir -p "$obj"

# Upstream's ee_printf.c ships an #error where the character sink belongs;
# substitute the one line rather than vendoring the other 700.
sed 's|^#error "You must implement the method uart_send_char.*|    gem5_putchar(c);|' \
    "$coremark_src/barebones/ee_printf.c" > "$obj/ee_printf.c"
grep -q 'gem5_putchar(c);' "$obj/ee_printf.c" || {
    echo "ee_printf.c stub not found; upstream changed, update the sed" >&2
    exit 1
}

flags_str="$guest_opt -march=$march -mabi=$mabi"
"$cc" $guest_opt -march="$march" -mabi="$mabi" \
    --specs=picolibc.specs -nostartfiles -ffreestanding \
    -I"$coremark_src" -I"$port_dir" -T "$port_dir/gem5.ld" \
    -DPERFORMANCE_RUN=1 -DITERATIONS="$iterations" \
    -DFLAGS_STR="\"$flags_str\"" \
    "$port_dir/crt.S" "$port_dir/gem5_port.c" "$port_dir/core_portme.c" \
    "$coremark_src/core_main.c" "$coremark_src/core_list_join.c" \
    "$coremark_src/core_matrix.c" "$coremark_src/core_state.c" \
    "$coremark_src/core_util.c" "$obj/ee_printf.c" \
    "$coremark_src/barebones/cvt.c" \
    -lm -o "$out"
echo "CoreMark ($iterations iterations, -march=$march): $out"
