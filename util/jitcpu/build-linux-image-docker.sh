#!/bin/sh
# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
#
# Build the JitCPU Linux artifacts in a Linux container. Sources and
# intermediate files live in a Docker volume because Linux has filenames that
# collide on the default case-insensitive macOS filesystem. Finished artifacts
# are copied to the host output directory, so Docker is not needed to boot.

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=${1:-"$repo_root/build/jitcpu-linux"}
builder_image=${JITCPU_LINUX_BUILDER_IMAGE:-gem5-jitcpu-linux-builder:ubuntu-24.04}
build_volume=${JITCPU_LINUX_BUILD_VOLUME:-gem5-jitcpu-linux-build}
host_uid=$(id -u)
host_gid=$(id -g)

mkdir -p "$build_dir"
build_dir=$(CDPATH= cd -- "$build_dir" && pwd)

docker build \
    --file "$repo_root/util/jitcpu/Dockerfile.linux-image" \
    --tag "$builder_image" \
    "$repo_root/util/jitcpu"

docker volume create "$build_volume" >/dev/null
docker run --rm \
    --user 0:0 \
    --mount "type=volume,src=$build_volume,dst=/work" \
    "$builder_image" \
    chown "$host_uid:$host_gid" /work

# Reuse source archives from an older host-mounted build if available. Only
# regular files are copied; source trees are deliberately extracted in the
# case-sensitive Linux volume.
docker run --rm \
    --user "$host_uid:$host_gid" \
    --env HOME=/tmp \
    --mount "type=volume,src=$build_volume,dst=/work" \
    --mount "type=bind,src=$build_dir,dst=/artifacts,readonly" \
    "$builder_image" \
    sh -c 'mkdir -p /work/src; if [ -d /artifacts/src ]; then find /artifacts/src -maxdepth 1 -type f -exec cp --update=none {} /work/src/ \;; fi'

docker run --rm \
    --user "$host_uid:$host_gid" \
    --env HOME=/tmp \
    --env BUSYBOX_VERSION \
    --env CROSS_COMPILE \
    --env JOBS \
    --env KERNEL_VERSION \
    --env MABI \
    --env MARCH \
    --env MUSL_VERSION \
    --mount "type=bind,src=$repo_root,dst=/gem5,readonly" \
    --mount "type=volume,src=$build_volume,dst=/work" \
    --workdir /gem5 \
    "$builder_image" \
    /gem5/util/jitcpu/build-linux-image.sh /work

docker run --rm \
    --user "$host_uid:$host_gid" \
    --mount "type=volume,src=$build_volume,dst=/work,readonly" \
    --mount "type=bind,src=$build_dir,dst=/artifacts" \
    "$builder_image" \
    sh -c 'set -eu
        cp /work/vmlinux /artifacts/vmlinux
        cp /work/fw_jump.elf /artifacts/fw_jump.elf
        cp /work/busybox-initramfs.cpio /artifacts/busybox-initramfs.cpio'

printf '%s\n' "JitCPU Linux artifacts exported to $build_dir"
