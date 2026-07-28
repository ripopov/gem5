#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GEM5_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
IMAGE="${IMAGE:-gem5-gpu-hsakmt-runner:local}"
BUILD_JOBS="${BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN)}"
PLATFORM="${PLATFORM:-}"

context="$(mktemp -d "${TMPDIR:-/tmp}/gem5-hsakmt-runner.XXXXXX")"
rocjitsu_context="$(
    mktemp -d "${TMPDIR:-/tmp}/rocjitsu-kmd-runner.XXXXXX"
)"
cleanup() {
    rm -rf "${context}"
    rm -rf "${rocjitsu_context}"
}
trap cleanup EXIT

# Start from the committed tree. git archive represents gitlinks as empty
# directories, so the large ROCm source submodule is not copied into this
# server-only context. Overlay all modified and non-ignored untracked files so
# an intentional local source change is still tested by the image build.
git -C "${GEM5_ROOT}" archive HEAD | tar -C "${context}" -xf -
while IFS= read -r -d '' path; do
    target="${context}/${path}"
    if [[ -f "${GEM5_ROOT}/${path}" || -L "${GEM5_ROOT}/${path}" ]]; then
        mkdir -p "$(dirname "${target}")"
        cp -Pp "${GEM5_ROOT}/${path}" "${target}"
    elif [[ ! -d "${GEM5_ROOT}/${path}" ]]; then
        rm -f "${target}"
    fi
done < <(
    git -C "${GEM5_ROOT}" diff --name-only -z HEAD
    git -C "${GEM5_ROOT}" ls-files -z --others --exclude-standard
)

ROCJITSU_ROOT="${GEM5_ROOT}/ext/hsakmt-client/rocm-systems"
git -C "${ROCJITSU_ROOT}" archive HEAD:emulation/rocjitsu \
    | tar -C "${rocjitsu_context}" -xf -
while IFS= read -r -d '' path; do
    case "${path}" in
        emulation/rocjitsu/*)
            relative="${path#emulation/rocjitsu/}"
            target="${rocjitsu_context}/${relative}"
            if [[ -f "${ROCJITSU_ROOT}/${path}" ||
                  -L "${ROCJITSU_ROOT}/${path}" ]]; then
                mkdir -p "$(dirname "${target}")"
                cp -Pp "${ROCJITSU_ROOT}/${path}" "${target}"
            elif [[ ! -d "${ROCJITSU_ROOT}/${path}" ]]; then
                rm -f "${target}"
            fi
            ;;
    esac
done < <(
    git -C "${ROCJITSU_ROOT}" diff --name-only -z HEAD
    git -C "${ROCJITSU_ROOT}" ls-files -z --others --exclude-standard
)

echo "Staged clean gem5 source context: $(du -sh "${context}" | cut -f1)" >&2
echo "Staged clean rocjitsu KMD context: $(du -sh "${rocjitsu_context}" | cut -f1)" >&2

build_args=(
    docker build
    --build-arg "BUILD_JOBS=${BUILD_JOBS}"
    --build-arg "GEM5_SOURCE_REVISION=$(git -C "${GEM5_ROOT}" rev-parse HEAD)"
    --build-arg "ROCJITSU_REVISION=$(git -C "${ROCJITSU_ROOT}" rev-parse HEAD)"
    --build-context "gem5-src=${context}"
    --build-context "rocjitsu-src=${rocjitsu_context}"
    --file "${SCRIPT_DIR}/Dockerfile"
    --tag "${IMAGE}"
)
if [[ -n "${PLATFORM}" ]]; then
    build_args+=(--platform "${PLATFORM}")
fi
build_args+=("${SCRIPT_DIR}")

"${build_args[@]}"
