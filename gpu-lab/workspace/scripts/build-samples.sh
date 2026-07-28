#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
    echo "usage: $0 --backend <simdojo|gem5-kmd|full-system>" >&2
}

backend=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --backend|--mode)
            [[ $# -ge 2 ]] || fail "$1 requires a value"
            backend="$(normalize_backend "$2")" || \
                fail "unknown backend: $2"
            shift 2
            ;;
        --backend=*|--mode=*)
            value="${1#*=}"
            backend="$(normalize_backend "${value}")" || \
                fail "unknown backend: ${value}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            exit 2
            ;;
    esac
done
[[ -n "${backend}" ]] || {
    usage
    exit 2
}

need_command docker
prepare_work_directories
out_dir="${BUNDLE_ROOT}/work/apps/${backend}"

case "${backend}" in
    full-system)
        need_docker_image "${FULL_SYSTEM_SDK_IMAGE}"
        docker run --rm --network none \
            --platform linux/amd64 \
            --user "$(id -u):$(id -g)" \
            -e HOME=/tmp \
            -e GPU_TARGET="${GPU_TARGET}" \
            -v "${BUNDLE_ROOT}:/workspace" \
            -w /workspace \
            "${FULL_SYSTEM_SDK_IMAGE}" \
            make -B -C samples all \
                OUT_DIR="/workspace/work/apps/${backend}" \
                GPU_TARGET="${GPU_TARGET}"
        ;;
    simdojo|gem5-kmd)
        need_docker_image "${HSAKMT_SDK_IMAGE}"
        export_hsakmt_platform
        docker run --rm --network none \
            --platform "${HSAKMT_PLATFORM}" \
            --user "$(id -u):$(id -g)" \
            -e HOME=/tmp \
            -e GPU_TARGET="${GPU_TARGET}" \
            -v "${BUNDLE_ROOT}:/workspace" \
            -w /workspace \
            "${HSAKMT_SDK_IMAGE}" \
            make -B -C samples all \
                OUT_DIR="/workspace/work/apps/${backend}" \
                GPU_TARGET="${GPU_TARGET}" \
                LLVM_OBJDUMP=/usr/bin/llvm-objdump-21
        ;;
esac

for sample in "${SAMPLES[@]}"; do
    need_file "${out_dir}/${sample}"
    need_file "${out_dir}/disasm/${sample}/kernels.${GPU_TARGET}.disasm"
done

echo "built ${backend} sample applications in ${out_dir}"
