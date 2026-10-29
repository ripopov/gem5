#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

need_command docker
need_file "${BUNDLE_ROOT}/images/gem5-gpu-fs-v25-1.tar.gz"
need_file "${BUNDLE_ROOT}/images/gem5-gpufs-v25.1.0.1.tar.gz"

docker load -i "${BUNDLE_ROOT}/images/gem5-gpu-fs-v25-1.tar.gz"
docker load -i "${BUNDLE_ROOT}/images/gem5-gpufs-v25.1.0.1.tar.gz"

need_docker_image "${FULL_SYSTEM_SDK_IMAGE}"
need_docker_image "gem5-gpufs:v25.1.0.1"
docker image tag gem5-gpufs:v25.1.0.1 gem5-gpufs-runtime:v25.1.0.1
need_docker_image "gem5-gpufs-runtime:v25.1.0.1"

echo "loaded ${FULL_SYSTEM_SDK_IMAGE}"
echo "loaded gem5-gpufs-runtime:v25.1.0.1 (optional legacy runtime)"
