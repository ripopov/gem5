#!/usr/bin/env bash

SCRIPT_DIR="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUNDLE_ROOT="$(cd -P "${SCRIPT_DIR}/.." && pwd)"
GEM5_ROOT="$(cd -P "${BUNDLE_ROOT}/../.." && pwd)"
COMPOSE_FILE="${BUNDLE_ROOT}/compose.yaml"

FULL_SYSTEM_IMAGE="${FULL_SYSTEM_IMAGE:-gem5-gpufs-runtime:v25.1.0.1}"
FULL_SYSTEM_SDK_IMAGE="${FULL_SYSTEM_SDK_IMAGE:-ghcr.io/gem5/gpu-fs:v25-1}"
HSAKMT_SDK_IMAGE="${HSAKMT_SDK_IMAGE:-gem5-gpu-hsakmt-sdk:local}"
HSAKMT_RUNNER_IMAGE="${HSAKMT_RUNNER_IMAGE:-gem5-gpu-hsakmt-runner:local}"
GEM5_EXE="${GEM5_EXE:-}"
GPU_TARGET="${GPU_TARGET:-gfx942}"
ROCJITSU_CONFIG="${ROCJITSU_CONFIG:-gfx942_cdna3_kmd.json}"

SAMPLES=(hip-vector-add hip-matmul hip-kernel-chain rainbow-image)

fail() {
    echo "error: $*" >&2
    exit 1
}

need_command() {
    command -v "$1" >/dev/null 2>&1 || \
        fail "required command is not installed: $1"
}

need_file() {
    [[ -f "$1" ]] || fail "missing required file: $1"
}

file_size() {
    wc -c < "$1" | tr -d '[:space:]'
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        fail "sha256sum or shasum is required"
    fi
}

normalize_backend() {
    case "$1" in
        simdojo|rocjitsu|rocjitsu-simdojo) echo simdojo ;;
        gem5-kmd|gem5|rocjitsu-gem5|hsakmt) echo gem5-kmd ;;
        full-system|gpufs) echo full-system ;;
        *) return 1 ;;
    esac
}

valid_sample() {
    local candidate="$1"
    local sample
    for sample in "${SAMPLES[@]}"; do
        [[ "${candidate}" == "${sample}" ]] && return 0
    done
    return 1
}

image_id() {
    local image="$1"
    local result

    if result="$(docker image inspect "${image}" \
        --format '{{.Id}}' 2>/dev/null)"; then
        printf '%s\n' "${result}"
        return 0
    fi

    # Docker Desktop may expose a tagged buildx image only through its ID.
    result="$(docker image ls --no-trunc --quiet "${image}" 2>/dev/null)"
    [[ -n "${result}" && "${result}" != *$'\n'* ]] || return 1
    printf '%s\n' "${result}"
}

need_docker_image() {
    image_id "$1" >/dev/null || \
        fail "required Docker image is not available locally: $1"
}

image_label() {
    local resolved
    resolved="$(image_id "$1")" || return 1
    docker image inspect "${resolved}" \
        --format "{{index .Config.Labels \"$2\"}}"
}

image_platform() {
    local resolved
    resolved="$(image_id "$1")" || return 1
    docker image inspect "${resolved}" --format '{{.Os}}/{{.Architecture}}'
}

normalize_platform() {
    local operating_system
    local architecture

    [[ "$1" == */* ]] || return 1
    operating_system="${1%%/*}"
    architecture="${1#*/}"
    architecture="${architecture%%/*}"
    case "${architecture}" in
        x86_64) architecture=amd64 ;;
        aarch64) architecture=arm64 ;;
    esac
    [[ -n "${operating_system}" && -n "${architecture}" ]] || return 1
    printf '%s/%s\n' "${operating_system}" "${architecture}"
}

export_hsakmt_platform() {
    local require_runner="${1:-false}"
    local sdk_platform
    local runner_platform
    local requested_platform

    sdk_platform="$(image_platform "${HSAKMT_SDK_IMAGE}")" || \
        fail "cannot determine platform for ${HSAKMT_SDK_IMAGE}"
    if [[ "${require_runner}" == true ]]; then
        runner_platform="$(image_platform "${HSAKMT_RUNNER_IMAGE}")" || \
            fail "cannot determine platform for ${HSAKMT_RUNNER_IMAGE}"
        [[ "${sdk_platform}" == "${runner_platform}" ]] || \
            fail "HSAKMT images have different platforms: ${sdk_platform} and ${runner_platform}"
    fi

    if [[ -n "${HSAKMT_PLATFORM:-}" ]]; then
        requested_platform="$(normalize_platform "${HSAKMT_PLATFORM}")" || \
            fail "invalid HSAKMT_PLATFORM: ${HSAKMT_PLATFORM}"
        [[ "${requested_platform}" == "${sdk_platform}" ]] || \
            fail "HSAKMT_PLATFORM=${HSAKMT_PLATFORM} does not match ${sdk_platform}"
    fi
    export HSAKMT_PLATFORM="${HSAKMT_PLATFORM:-${sdk_platform}}"
}

require_matching_rocjitsu() {
    local sdk_revision
    local runner_revision

    sdk_revision="$(image_label \
        "${HSAKMT_SDK_IMAGE}" org.gem5.rocjitsu.revision)" || \
        fail "cannot read the rocjitsu revision from ${HSAKMT_SDK_IMAGE}"
    runner_revision="$(image_label \
        "${HSAKMT_RUNNER_IMAGE}" org.gem5.rocjitsu.revision)" || \
        fail "cannot read the rocjitsu revision from ${HSAKMT_RUNNER_IMAGE}"
    [[ -n "${sdk_revision}" && "${sdk_revision}" != "<no value>" ]] || \
        fail "${HSAKMT_SDK_IMAGE} does not record a rocjitsu revision"
    [[ "${sdk_revision}" == "${runner_revision}" ]] || \
        fail "HSAKMT images contain different rocjitsu revisions"
}

export_host_identity() {
    export HOST_UID="${HOST_UID:-$(id -u)}"
    export HOST_GID="${HOST_GID:-$(id -g)}"
    [[ "${HOST_UID}" =~ ^[0-9]+$ ]] || fail "HOST_UID must be numeric"
    [[ "${HOST_GID}" =~ ^[0-9]+$ ]] || fail "HOST_GID must be numeric"
}

lab_compose() {
    docker compose \
        --project-directory "${BUNDLE_ROOT}" \
        --file "${COMPOSE_FILE}" \
        "$@"
}

prepare_work_directories() {
    mkdir -p \
        "${BUNDLE_ROOT}/work/apps" \
        "${BUNDLE_ROOT}/work/runs"
}

new_run_id() {
    local backend="$1"
    local name="$2"
    printf '%s-%s-%s-%s-%s\n' \
        "${name}" "${backend}" "$(date -u +%Y%m%dT%H%M%SZ)" "$$" "${RANDOM}"
}

validate_run_id() {
    [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9_.-]*$ ]] || \
        fail "run id contains unsupported characters: $1"
}

sample_app_path() {
    printf '%s/work/apps/%s/%s\n' "${BUNDLE_ROOT}" "$1" "$2"
}

expected_machine() {
    case "$(normalize_platform "$1")" in
        linux/amd64) echo "Advanced Micro Devices X86-64" ;;
        linux/arm64) echo "AArch64" ;;
        *) fail "unsupported executable platform: $1" ;;
    esac
}

verify_container_executable() {
    local image="$1"
    local platform="$2"
    local container_path="$3"
    local machine_line

    machine_line="$(docker run --rm --network none \
        --platform "${platform}" \
        --entrypoint /bin/sh \
        -v "${BUNDLE_ROOT}:/workspace:ro" \
        "${image}" -c 'readelf -h "$1" | grep "Machine:"' sh \
        "${container_path}")" || \
        fail "cannot inspect executable: ${container_path}"
    [[ "${machine_line}" == *"$(expected_machine "${platform}")"* ]] || \
        fail "${container_path} does not match ${platform}; rebuild it for this backend"
}
