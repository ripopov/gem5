#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GEM5_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
ROCM_SYSTEMS_ROOT="${GEM5_ROOT}/ext/hsakmt-client/rocm-systems"
IMAGE="${IMAGE:-gem5-gpu-hsakmt-sdk:local}"
BUILD_JOBS="${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}"
PLATFORM="${PLATFORM:-}"

if [[ ! -f "${ROCM_SYSTEMS_ROOT}/projects/rocr-runtime/CMakeLists.txt" ]]; then
    echo "ROCr source is missing; initialize ext/hsakmt-client/rocm-systems" >&2
    exit 1
fi

if [[ ! -f "${ROCM_SYSTEMS_ROOT}/emulation/rocjitsu/CMakeLists.txt" ]]; then
    echo "rocjitsu source is missing; initialize ext/hsakmt-client/rocm-systems" >&2
    exit 1
fi

GEM5_SOURCE_REVISION="$(git -C "${GEM5_ROOT}" rev-parse HEAD)"
ROCR_REVISION="$(git -C "${ROCM_SYSTEMS_ROOT}" rev-parse HEAD)"

source_context="$(
    mktemp -d "${TMPDIR:-/tmp}/gpu-hsakmt-sdk-sources.XXXXXX"
)"
cleanup() {
    rm -rf "${source_context}"
}
trap cleanup EXIT

mkdir -p \
    "${source_context}/projects/rocr-runtime" \
    "${source_context}/emulation/rocjitsu"
git -C "${ROCM_SYSTEMS_ROOT}" archive HEAD:projects/rocr-runtime \
    | tar -C "${source_context}/projects/rocr-runtime" -xf -
git -C "${ROCM_SYSTEMS_ROOT}" archive HEAD:emulation/rocjitsu \
    | tar -C "${source_context}/emulation/rocjitsu" -xf -

# Overlay source changes from the working submodule so image verification
# exercises the exact rocjitsu implementation under review.
while IFS= read -r -d '' path; do
    case "${path}" in
        projects/rocr-runtime/*|emulation/rocjitsu/*)
            target="${source_context}/${path}"
            if [[ -f "${ROCM_SYSTEMS_ROOT}/${path}" ||
                  -L "${ROCM_SYSTEMS_ROOT}/${path}" ]]; then
                mkdir -p "$(dirname "${target}")"
                cp -Pp "${ROCM_SYSTEMS_ROOT}/${path}" "${target}"
            elif [[ ! -d "${ROCM_SYSTEMS_ROOT}/${path}" ]]; then
                rm -f "${target}"
            fi
            ;;
    esac
done < <(
    git -C "${ROCM_SYSTEMS_ROOT}" diff --name-only -z HEAD
    git -C "${ROCM_SYSTEMS_ROOT}" ls-files -z --others --exclude-standard
)

build_args=(docker build \
    --build-arg "BUILD_JOBS=${BUILD_JOBS}" \
    --build-arg "GEM5_SOURCE_REVISION=${GEM5_SOURCE_REVISION}" \
    --build-arg "ROCR_REVISION=${ROCR_REVISION}" \
    --build-arg "ROCJITSU_REVISION=${ROCR_REVISION}" \
    --build-context "rocm-systems=${source_context}" \
    --file "${SCRIPT_DIR}/Dockerfile" \
    --tag "${IMAGE}")
if [[ -n "${PLATFORM}" ]]; then
    build_args+=(--platform "${PLATFORM}")
fi
build_args+=("${SCRIPT_DIR}")

"${build_args[@]}"
