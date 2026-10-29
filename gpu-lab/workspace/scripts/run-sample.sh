#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
    cat >&2 <<EOF
usage: $0 --backend <simdojo|gem5-kmd|full-system> [--gem5-exe PATH] <sample>
EOF
}

backend=""
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
        --)
            shift
            break
            ;;
        *)
            break
            ;;
    esac
done

[[ -n "${backend}" && $# -eq 1 ]] || {
    usage
    exit 2
}
[[ "${backend}" == full-system || ${#run_args[@]} -eq 0 ]] || \
    fail "--gem5-exe applies only to the full-system backend"

sample="$1"
valid_sample "${sample}" || fail "unknown sample: ${sample}"
app="$(sample_app_path "${backend}" "${sample}")"
need_file "${app}"

run_id="$(new_run_id "${backend}" "${sample}")"
validate_run_id "${run_id}"
export GPU_LAB_RUN_ID="${run_id}"

"${SCRIPT_DIR}/run-app.sh" \
    --backend "${backend}" "${run_args[@]}" --run-name "${sample}" "${app}"

run_dir="${BUNDLE_ROOT}/work/runs/${run_id}"
"${SCRIPT_DIR}/verify-run.sh" \
    --backend "${backend}" "${sample}" "${run_dir}"
