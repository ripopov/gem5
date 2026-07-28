#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
    echo "usage: $0 --backend <simdojo|gem5-kmd|full-system> <sample> <run-dir>" >&2
}

backend=""
case "${1:-}" in
    -h|--help)
        usage
        exit 0
        ;;
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
esac
[[ -n "${backend}" && $# -eq 2 ]] || {
    usage
    exit 2
}

sample="$1"
out_dir="$2"
valid_sample "${sample}" || fail "unknown sample: ${sample}"

case "${sample}" in
    hip-vector-add)
        expected_output="hip_vector_add: PASS checksum=6048"
        expected_kernels=4
        ;;
    hip-matmul)
        expected_output="hip_matmul: PASS n=128 layout=row-major-b checksum=25690112"
        expected_kernels=1
        ;;
    hip-kernel-chain)
        expected_output="hip_kernel_chain: PASS checksum=40768"
        expected_kernels=5
        ;;
    rainbow-image)
        expected_output="rainbow_image: PASS image=rainbow-image.bmp"
        expected_kernels=1
        ;;
esac

need_file "${out_dir}/exit-status.txt"

case "${backend}" in
    full-system)
        grep -Fqx "gem5=0" "${out_dir}/exit-status.txt" || \
            fail "full-system gem5 did not exit successfully: ${out_dir}"
        need_file "${out_dir}/stats.txt"
        [[ -s "${out_dir}/stats.txt" ]] || \
            fail "stats.txt is empty: ${out_dir}"
        logs=("${out_dir}/gem5.log" "${out_dir}/system.pc.com_1.device")
        ;;
    simdojo)
        grep -Fqx "application=0" "${out_dir}/exit-status.txt" || \
            fail "application did not exit successfully: ${out_dir}"
        logs=("${out_dir}/app.stdout")
        ;;
    gem5-kmd)
        grep -Fqx "application=0" "${out_dir}/exit-status.txt" || \
            fail "application did not exit successfully: ${out_dir}"
        need_file "${out_dir}/stats.txt"
        [[ -s "${out_dir}/stats.txt" ]] || \
            fail "stats.txt is empty: ${out_dir}"
        need_file "${out_dir}/gem5.log"
        grep -Fqx "gem5=0" "${out_dir}/exit-status.txt" || \
            fail "gem5 did not exit successfully: ${out_dir}"
        grep -Fqx "timed_out=false" "${out_dir}/exit-status.txt" || \
            fail "gem5 timed out: ${out_dir}"
        grep -Fq "rocjitsu KMD state drained" "${out_dir}/gem5.log" || \
            fail "gem5 KMD state did not drain: ${out_dir}"
        grep -Fq "because rocjitsu KMD client disconnected" \
            "${out_dir}/gem5.log" || \
            fail "gem5 did not report a clean client disconnect: ${out_dir}"
        if grep -Eq 'fatal:|panic:|KMD reactor failed|dropped stale doorbell' \
                "${out_dir}/gem5.log"; then
            fail "gem5 log contains a fatal KMD or simulator error: ${out_dir}"
        fi

        launched="$(awk \
            '$1 == "system.cpu1.gpu_cmd_proc.dispatcher.numKernelLaunched" {print $2}' \
            "${out_dir}/stats.txt")"
        submitted="$(awk \
            '$1 == "system.cpu1.gpu_cmd_proc.hsapp.aqlPacketsSubmitted" {print $2}' \
            "${out_dir}/stats.txt")"
        retired="$(awk \
            '$1 == "system.cpu1.gpu_cmd_proc.hsapp.aqlPacketsRetired" {print $2}' \
            "${out_dir}/stats.txt")"
        [[ "${launched}" == "${expected_kernels}" ]] || \
            fail "expected ${expected_kernels} modeled kernels, found ${launched:-none}"
        [[ -n "${submitted}" && "${submitted}" != 0 &&
           "${retired}" == "${submitted}" ]] || \
            fail "AQL packets did not drain: submitted=${submitted:-none}, retired=${retired:-none}"
        logs=("${out_dir}/app.stdout")
        ;;
esac

found=false
for log in "${logs[@]}"; do
    if [[ -f "${log}" ]] && grep -Fq "${expected_output}" "${log}"; then
        found=true
        break
    fi
done
${found} || \
    fail "did not find expected output '${expected_output}' in ${out_dir}"

if [[ "${sample}" == rainbow-image ]]; then
    bmp="${out_dir}/rainbow-image.bmp"
    need_file "${bmp}"
    [[ "$(file_size "${bmp}")" == 230454 ]] || \
        fail "rainbow-image BMP has the wrong size"
    IFS= read -r -n 2 magic < "${bmp}"
    [[ "${magic}" == BM ]] || fail "rainbow-image output is not a BMP"
    width="$(od -An -t u4 -j 18 -N 4 "${bmp}")"
    height="$(od -An -t u4 -j 22 -N 4 "${bmp}")"
    width="${width//[[:space:]]/}"
    height="${height//[[:space:]]/}"
    [[ "${width}x${height}" == 320x240 ]] || \
        fail "rainbow-image BMP dimensions are ${width}x${height}"
    case "${backend}" in
        simdojo)
            expected_digest="4f1ec216b27cc2fb00c257f67a0eb4c74d50983ce3665172ef413073e6c1221a"
            ;;
        gem5-kmd|full-system)
            expected_digest="fb5693554ce9416c56c62c2d3ba422fd32c8a8fb8cf0807b8c74127c05786e00"
            ;;
    esac
    digest="$(sha256_file "${bmp}")"
    [[ "${digest}" == "${expected_digest}" ]] || \
        fail "rainbow-image checksum ${digest} does not match ${backend}"
fi

echo "${sample}: PASS (${backend}, ${out_dir})"
