#!/bin/sh
# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=${1:-"$repo_root/build/qemu-jit"}
qemu_repo="$repo_root/ext/qemu/repo"

case "$(uname -s)" in
    Darwin)
        backend_library="libgem5-qemu-jit.dylib"
        ;;
    *)
        backend_library="libgem5-qemu-jit.so"
        ;;
esac

mkdir -p "$build_dir"
build_dir=$(CDPATH= cd -- "$build_dir" && pwd)
source_dir="$build_dir.qemu-source"
source_stamp="$build_dir/.gem5-jit-source"

if [ ! -f "$qemu_repo/configure" ]; then
    printf '%s\n' \
        "QEMU is not initialized at $qemu_repo" \
        "Run: git submodule update --init ext/qemu/repo" >&2
    exit 1
fi

source_id=$(git -C "$qemu_repo" rev-parse HEAD)

if [ ! -f "$build_dir/build.ninja" ]; then
    if [ -e "$source_dir" ]; then
        printf '%s\n' \
            "Stale QEMU source snapshot exists at $source_dir" \
            "Remove it and rerun the build helper." >&2
        exit 1
    fi

    mkdir -p "$source_dir"
    tar -C "$qemu_repo" --exclude='.git' --exclude='*/.git' -cf - . | \
        tar -xf - -C "$source_dir"

    cd "$build_dir"
    "$source_dir/configure" \
        --target-list=riscv64-softmmu \
        --disable-docs \
        --disable-tools \
        --disable-guest-agent \
        --disable-plugins \
        --disable-rust \
        --disable-werror \
        --disable-slirp \
        -Db_staticpic=true
    printf '%s\n' "$source_id" > "$source_stamp"
elif [ ! -f "$source_stamp" ] ||
     [ "$(cat "$source_stamp")" != "$source_id" ]; then
    printf '%s\n' \
        "The QEMU revision changed." \
        "Remove $build_dir and $source_dir, then rerun the build helper." >&2
    exit 1
fi

ninja -C "$build_dir" "$backend_library" gem5-qemu-jit-smoke
"$build_dir/gem5-qemu-jit-smoke"

printf '%s\n' "$build_dir/$backend_library"
