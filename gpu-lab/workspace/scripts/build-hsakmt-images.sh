#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

HSAKMT_SDK_IMAGE="${HSAKMT_SDK_IMAGE:-gem5-gpu-hsakmt-sdk:local}"
HSAKMT_RUNNER_IMAGE="${HSAKMT_RUNNER_IMAGE:-gem5-gpu-hsakmt-runner:local}"
HSAKMT_PLATFORM="${HSAKMT_PLATFORM:-linux/amd64}"
BUILD_JOBS="${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}"

build_sdk=true
build_runner=true
case "${1:-}" in
    "") ;;
    --sdk-only) build_runner=false ;;
    --runner-only) build_sdk=false ;;
    *)
        echo "usage: $0 [--sdk-only|--runner-only]" >&2
        exit 2
        ;;
esac

need_command docker
docker compose version >/dev/null

if ${build_sdk}; then
    IMAGE="${HSAKMT_SDK_IMAGE}" PLATFORM="${HSAKMT_PLATFORM}" \
        BUILD_JOBS="${BUILD_JOBS}" \
        "${GEM5_ROOT}/util/dockerfiles/gpu-hsakmt-sdk/build.sh"
fi
if ${build_runner}; then
    IMAGE="${HSAKMT_RUNNER_IMAGE}" PLATFORM="${HSAKMT_PLATFORM}" \
        BUILD_JOBS="${BUILD_JOBS}" \
        "${GEM5_ROOT}/util/dockerfiles/gpu-hsakmt-runner/build.sh"
fi

revision="$(git -C "${GEM5_ROOT}" rev-parse HEAD)"
if ${build_sdk}; then
    resolved_image="$(image_id "${HSAKMT_SDK_IMAGE}")"
    actual="$(docker image inspect "${resolved_image}" \
        --format '{{index .Config.Labels "org.gem5.hsakmt.sdk.revision"}}')"
    [[ "${actual}" == "${revision}" ]] || \
        fail "SDK image revision ${actual} does not match ${revision}"
fi
if ${build_runner}; then
    resolved_image="$(image_id "${HSAKMT_RUNNER_IMAGE}")"
    actual="$(docker image inspect "${resolved_image}" \
        --format '{{index .Config.Labels "org.gem5.hsakmt.server.revision"}}')"
    [[ "${actual}" == "${revision}" ]] || \
        fail "runner image revision ${actual} does not match ${revision}"
fi

export HSAKMT_SDK_IMAGE HSAKMT_RUNNER_IMAGE HSAKMT_PLATFORM
if ${build_sdk} && ${build_runner}; then
    require_matching_rocjitsu
fi
export_host_identity
lab_compose config --quiet
echo "HSAKMT images are ready for ${HSAKMT_PLATFORM}."
