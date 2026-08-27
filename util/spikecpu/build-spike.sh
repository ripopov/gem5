#!/bin/sh
# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=${1:-"$repo_root/build/spike"}
spike_repo="$repo_root/ext/spike/repo"
adapter_dir="$repo_root/ext/spike/gem5-spike"

case "$(uname -s)" in
    Darwin)
        backend_library="libgem5-spike.dylib"
        ;;
    *)
        backend_library="libgem5-spike.so"
        ;;
esac

if [ ! -f "$spike_repo/configure" ]; then
    printf '%s\n' \
        "Spike is not initialized at $spike_repo" \
        "Run: git submodule update --init ext/spike/repo" >&2
    exit 1
fi

mkdir -p "$build_dir"
build_dir=$(CDPATH= cd -- "$build_dir" && pwd)
source_stamp="$build_dir/.gem5-spike-source"
source_id=$(git -C "$spike_repo" rev-parse HEAD)

# Spike builds out of tree, so unlike the QEMU backend no source snapshot is
# needed; only the configuration has to be redone when the revision moves.
if [ ! -f "$build_dir/Makefile" ]; then
    (cd "$build_dir" && "$spike_repo/configure" \
        --prefix="$build_dir/install" > "$build_dir/configure.log" 2>&1)
    printf '%s\n' "$source_id" > "$source_stamp"
elif [ ! -f "$source_stamp" ] ||
     [ "$(cat "$source_stamp")" != "$source_id" ]; then
    printf '%s\n' \
        "The Spike revision changed." \
        "Remove $build_dir, then rerun the build helper." >&2
    exit 1
fi

make -s -C "$build_dir" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
    libriscv.so

# Spike is BSD licensed, so unlike QEMU it could be linked into gem5
# directly. It is loaded the same way anyway: gem5 then depends on one C ABI
# rather than on Spike's C++ headers and autotools build, and the two
# functional backends stay interchangeable at run time.
${CXX:-g++} -std=c++2a -O2 -fPIC -shared \
    -o "$build_dir/$backend_library" \
    "$adapter_dir/gem5-spike.cc" \
    -I"$adapter_dir" \
    -I"$spike_repo" -I"$spike_repo/riscv" -I"$spike_repo/softfloat" \
    -I"$build_dir" \
    -L"$build_dir" -lriscv -Wl,-rpath,'$ORIGIN'

${CXX:-g++} -std=c++2a -O2 -o "$build_dir/gem5-spike-smoke" \
    "$adapter_dir/gem5-spike-smoke.cc" \
    -I"$adapter_dir" \
    -L"$build_dir" -lgem5-spike -Wl,-rpath,"$build_dir"

"$build_dir/gem5-spike-smoke"

printf '%s\n' "$build_dir/$backend_library"
