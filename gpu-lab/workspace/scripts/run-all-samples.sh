#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
    cat >&2 <<EOF
usage: $0 --backend <simdojo|gem5-kmd|full-system> [--no-build] [--gem5-exe PATH]
EOF
}

backend=""
build=true
run_args=()
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
        --no-build)
            build=false
            shift
            ;;
        --gem5-exe)
            [[ $# -ge 2 ]] || fail "--gem5-exe requires a path"
            run_args+=(--gem5-exe "$2")
            shift 2
            ;;
        --gem5-exe=*)
            run_args+=("$1")
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            exit 1
            ;;
    esac
done
[[ -n "${backend}" ]] || {
    usage
    exit 2
}
if [[ "${backend}" != full-system && ${#run_args[@]} -ne 0 ]]; then
    fail "--gem5-exe applies only to the full-system backend"
fi

if ${build}; then
    "${SCRIPT_DIR}/build-samples.sh" --backend "${backend}"
fi

for sample in "${SAMPLES[@]}"; do
    "${SCRIPT_DIR}/run-sample.sh" \
        --backend "${backend}" "${run_args[@]}" "${sample}"
done

echo "all ${backend} samples passed"
