#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

need_command docker
need_file "${BUNDLE_ROOT}/images/app-build-gpu-fs-v25-1.tar.gz"
need_file "${BUNDLE_ROOT}/images/gem5-runtime-v25.1.0.1.tar.gz"

docker load -i "${BUNDLE_ROOT}/images/app-build-gpu-fs-v25-1.tar.gz"
docker load -i "${BUNDLE_ROOT}/images/gem5-runtime-v25.1.0.1.tar.gz"

need_docker_image "${FULL_SYSTEM_SDK_IMAGE}"
need_docker_image "${FULL_SYSTEM_IMAGE}"

echo "loaded ${FULL_SYSTEM_SDK_IMAGE}"
echo "loaded ${FULL_SYSTEM_IMAGE}"
