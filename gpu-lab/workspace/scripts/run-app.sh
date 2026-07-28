#!/usr/bin/env bash
set -euo pipefail

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
    cat >&2 <<EOF
usage: $0 --backend <simdojo|gem5-kmd|full-system> [options] <app> [app args...] [-- config args...]

Options:
  --run-name NAME    Label used in the run-directory name.
  --gem5-exe PATH    Use a host gem5 executable in full-system mode.
  --gem5-arg ARG     Pass one top-level gem5 argument; may be repeated.

Backends:
  simdojo           Run through rocjitsu on the functional simdojo model.
  gem5-kmd          Run through rocjitsu attached to gem5's HSAKMT service.
  full-system       Boot Linux and run through the guest ROCm/KFD stack.

Arguments after -- are passed to the selected gem5 Python configuration.
The legacy --mode spelling is accepted as an alias for --backend.
EOF
}

resolve_gem5_exe() {
    local path="$1"
    local repo_candidate="${GEM5_ROOT}/${path}"

    if [[ -e "${path}" ]]; then
        realpath "${path}"
        return
    fi
    if [[ -e "${repo_candidate}" ]]; then
        realpath "${repo_candidate}"
        return
    fi
    fail "missing gem5 executable: ${path}"
}

find_gem5_root() {
    local directory="$1"
    while [[ "${directory}" != / ]]; do
        if [[ -f "${directory}/configs/example/gpufs/mi300.py" ]]; then
            echo "${directory}"
            return
        fi
        directory="$(dirname "${directory}")"
    done
    fail "could not find a gem5 source root above $1"
}

quote_shell_args() {
    local argument
    local escaped
    local joined=""

    for argument in "$@"; do
        escaped="${argument//\'/\'\\\'\'}"
        joined+="${joined:+ }'${escaped}'"
    done
    printf '%s\n' "${joined}"
}

backend=""
run_name=""
gem5_args=()
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
        --run-name)
            [[ $# -ge 2 ]] || fail "--run-name requires a value"
            run_name="$2"
            shift 2
            ;;
        --run-name=*)
            run_name="${1#*=}"
            shift
            ;;
        --gem5-exe)
            [[ $# -ge 2 ]] || fail "--gem5-exe requires a path"
            GEM5_EXE="$2"
            shift 2
            ;;
        --gem5-exe=*)
            GEM5_EXE="${1#*=}"
            shift
            ;;
        --gem5-arg)
            [[ $# -ge 2 ]] || fail "--gem5-arg requires a value"
            gem5_args+=("$2")
            shift 2
            ;;
        --gem5-arg=*)
            gem5_args+=("${1#*=}")
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
        -*)
            fail "unknown option: $1"
            ;;
        *)
            break
            ;;
    esac
done

[[ -n "${backend}" && $# -ge 1 ]] || {
    usage
    exit 2
}
[[ "${backend}" == full-system || -z "${GEM5_EXE}" ]] || \
    fail "--gem5-exe applies only to the full-system backend"

app_path="$1"
shift
app_args=()
config_args=()
while [[ $# -gt 0 ]]; do
    if [[ "$1" == -- ]]; then
        shift
        config_args=("$@")
        break
    fi
    app_args+=("$1")
    shift
done
[[ "${backend}" != simdojo || ${#gem5_args[@]} -eq 0 ]] || \
    fail "the simdojo backend does not accept --gem5-arg"
[[ "${backend}" != simdojo || ${#config_args[@]} -eq 0 ]] || \
    fail "the simdojo backend does not accept arguments after --"

need_command docker
need_file "${app_path}"
app_abs="$(realpath "${app_path}")"
case "${app_abs}" in
    "${BUNDLE_ROOT}"/*) app_rel="${app_abs#"${BUNDLE_ROOT}/"}" ;;
    *) fail "application must be inside ${BUNDLE_ROOT}: ${app_abs}" ;;
esac
if [[ "${backend}" == gem5-kmd && "${app_rel}" != work/apps/* ]]; then
    fail "gem5-kmd applications must be below ${BUNDLE_ROOT}/work/apps"
fi

run_name="${run_name:-$(basename "${app_path}")}"
run_id="${GPU_LAB_RUN_ID:-$(new_run_id "${backend}" "${run_name}")}"
validate_run_id "${run_id}"
prepare_work_directories
out_dir="${BUNDLE_ROOT}/work/runs/${run_id}"
[[ ! -e "${out_dir}" ]] || fail "run id already exists: ${run_id}"
mkdir -p "${out_dir}"

{
    echo "backend=${backend}"
    echo "run_id=${run_id}"
    echo "application=${app_rel}"
    echo "gpu_target=${GPU_TARGET}"
} > "${out_dir}/run.env"

if [[ "${backend}" != simdojo ]]; then
    : > "${out_dir}/gem5.args"
    if [[ ${#gem5_args[@]} -gt 0 ]]; then
        printf '%s\0' "${gem5_args[@]}" > "${out_dir}/gem5.args"
    fi
    : > "${out_dir}/config.args"
    if [[ ${#config_args[@]} -gt 0 ]]; then
        printf '%s\0' "${config_args[@]}" > "${out_dir}/config.args"
    fi
fi

run_full_system() {
    need_docker_image "${FULL_SYSTEM_IMAGE}"
    local disk_image="${BUNDLE_ROOT}/disk/x86-ubuntu-rocm70"
    local kernel_image="${BUNDLE_ROOT}/kernel/vmlinux-rocm70"
    need_file "${disk_image}"
    need_file "${kernel_image}"
    disk_image="$(realpath "${disk_image}")"
    kernel_image="$(realpath "${kernel_image}")"
    [[ -c /dev/kvm ]] || \
        fail "/dev/kvm is unavailable; full-system uses X86KvmCPU"

    local gem5_command="build/VEGA_X86/gem5.opt"
    local gem5_description="bundled ${gem5_command} from ${FULL_SYSTEM_IMAGE}"
    local gem5_workdir="/opt/gem5"
    local -a docker_command=(
        docker run --rm --network none
        --platform linux/amd64
        --device /dev/kvm
        -v "${BUNDLE_ROOT}:/workspace"
        -v "${disk_image}:/gpu-lab-artifacts/disk-image:ro"
        -v "${kernel_image}:/gpu-lab-artifacts/kernel:ro"
    )

    if [[ -n "${GEM5_EXE}" ]]; then
        local host_executable
        local host_root
        local executable_relative
        host_executable="$(resolve_gem5_exe "${GEM5_EXE}")"
        need_file "${host_executable}"
        [[ -x "${host_executable}" ]] || \
            fail "gem5 executable is not executable: ${host_executable}"
        host_root="$(find_gem5_root "$(dirname "${host_executable}")")"
        case "${host_executable}" in
            "${host_root}"/*)
                executable_relative="${host_executable#"${host_root}/"}"
                ;;
            *) fail "gem5 executable is outside ${host_root}" ;;
        esac
        gem5_command="/gem5-local/${executable_relative}"
        gem5_workdir="/gem5-local"
        gem5_description="host ${host_executable}"
        docker_command+=(-v "${host_root}:/gem5-local:ro")
    fi

    local -a command=(
        "${gem5_command}"
        -d "/workspace/work/runs/${run_id}"
        "${gem5_args[@]}"
        configs/example/gpufs/mi300.py
        "${config_args[@]}"
        --disk-image /gpu-lab-artifacts/disk-image
        --kernel /gpu-lab-artifacts/kernel
        --app "/workspace/${app_rel}"
    )
    if [[ ${#app_args[@]} -gt 0 ]]; then
        command+=(--opts "$(quote_shell_args "${app_args[@]}")")
    fi
    docker_command+=(
        -w "${gem5_workdir}"
        "${FULL_SYSTEM_IMAGE}"
        "${command[@]}"
    )

    set +e
    {
        echo "gem5 executable: ${gem5_description}"
        "${docker_command[@]}"
    } 2>&1 | tee "${out_dir}/gem5.log"
    local status=${PIPESTATUS[0]}
    set -e
    printf 'gem5=%s\n' "${status}" > "${out_dir}/exit-status.txt"
    [[ "${status}" -eq 0 ]] || \
        fail "full-system simulation exited with status ${status}: ${out_dir}"
}

run_simdojo() {
    need_docker_image "${HSAKMT_SDK_IMAGE}"
    export_hsakmt_platform
    verify_container_executable \
        "${HSAKMT_SDK_IMAGE}" "${HSAKMT_PLATFORM}" \
        "/workspace/${app_rel}"

    set +e
    docker run --rm --network none \
        --platform "${HSAKMT_PLATFORM}" \
        --user "$(id -u):$(id -g)" \
        -e HOME=/tmp \
        -e ROCJITSU_CONFIG="${ROCJITSU_CONFIG}" \
        -v "${BUNDLE_ROOT}:/workspace:ro" \
        -v "${out_dir}:/tmp" \
        -w /tmp \
        "${HSAKMT_SDK_IMAGE}" \
        rocjitsu-run "/workspace/${app_rel}" "${app_args[@]}" \
        > "${out_dir}/app.stdout" 2> "${out_dir}/app.stderr"
    local status=$?
    set -e
    printf 'application=%s\n' "${status}" > "${out_dir}/exit-status.txt"
    if [[ "${status}" -ne 0 ]]; then
        cat "${out_dir}/app.stderr" >&2
        fail "simdojo application exited with status ${status}: ${out_dir}"
    fi
}

run_gem5_kmd() {
    docker compose version >/dev/null
    need_docker_image "${HSAKMT_SDK_IMAGE}"
    need_docker_image "${HSAKMT_RUNNER_IMAGE}"
    export_hsakmt_platform true
    require_matching_rocjitsu
    export_host_identity
    verify_container_executable \
        "${HSAKMT_SDK_IMAGE}" "${HSAKMT_PLATFORM}" \
        "/workspace/${app_rel}"

    local sdk_app="/workspace/apps/${app_rel#work/apps/}"
    export HSAKMT_SDK_IMAGE HSAKMT_RUNNER_IMAGE HSAKMT_PLATFORM
    export HSAKMT_RUN_ID="${run_id}"
    export COMPOSE_PROJECT_NAME="gpu-lab-$(
        printf '%s' "${run_id}" | tr '[:upper:]_.' '[:lower:]--'
    )"
    {
        echo "compose_project=${COMPOSE_PROJECT_NAME}"
        echo "platform=${HSAKMT_PLATFORM}"
        echo "sdk_image=${HSAKMT_SDK_IMAGE}"
        echo "runner_image=${HSAKMT_RUNNER_IMAGE}"
    } >> "${out_dir}/run.env"

    cleanup_gem5_kmd() {
        local status=$?
        if [[ ! -s "${out_dir}/gem5.log" ]]; then
            lab_compose logs --no-color --no-log-prefix gem5 \
                > "${out_dir}/gem5.log" 2>&1 || true
        fi
        lab_compose down --volumes --remove-orphans >/dev/null 2>&1 || true
        return "${status}"
    }
    trap cleanup_gem5_kmd EXIT

    lab_compose config --quiet
    lab_compose up -d --wait \
        --wait-timeout "${HSAKMT_READY_TIMEOUT:-180}" gem5

    local -a application_command=(
        run --rm --no-deps
        --volume "${out_dir}:/tmp"
        sdk rocjitsu-run --attach
        "${sdk_app}" "${app_args[@]}"
    )

    set +e
    lab_compose "${application_command[@]}" \
        > "${out_dir}/app.stdout" 2> "${out_dir}/app.stderr"
    local application_status=$?
    set -e

    local container_id
    container_id="$(lab_compose ps --all -q gem5)"
    [[ -n "${container_id}" ]] || fail "cannot find the gem5 container"
    local timeout="${HSAKMT_GEM5_EXIT_TIMEOUT:-180}"
    [[ "${timeout}" =~ ^[0-9]+$ ]] || \
        fail "HSAKMT_GEM5_EXIT_TIMEOUT must be numeric"
    local deadline=$((SECONDS + timeout))
    local timed_out=false
    while [[ "$(docker inspect --format '{{.State.Running}}' \
            "${container_id}")" == true ]]; do
        if (( SECONDS >= deadline )); then
            timed_out=true
            lab_compose stop --timeout 10 gem5 >/dev/null
            break
        fi
        sleep 1
    done

    local gem5_status
    gem5_status="$(
        docker inspect --format '{{.State.ExitCode}}' "${container_id}"
    )"
    lab_compose logs --no-color --no-log-prefix gem5 \
        > "${out_dir}/gem5.log" 2>&1
    {
        echo "application=${application_status}"
        echo "gem5=${gem5_status}"
        echo "timed_out=${timed_out}"
    } > "${out_dir}/exit-status.txt"

    [[ "${application_status}" -eq 0 ]] || {
        cat "${out_dir}/app.stderr" >&2
        fail "gem5-kmd application exited with status ${application_status}: ${out_dir}"
    }
    ${timed_out} && fail "gem5 did not exit after client disconnect: ${out_dir}"
    [[ "${gem5_status}" -eq 0 ]] || {
        tail -n 80 "${out_dir}/gem5.log" >&2
        fail "gem5 exited with status ${gem5_status}: ${out_dir}"
    }

    lab_compose down --volumes --remove-orphans >/dev/null
    trap - EXIT
}

case "${backend}" in
    simdojo) run_simdojo ;;
    gem5-kmd) run_gem5_kmd ;;
    full-system) run_full_system ;;
esac

echo "run output: ${out_dir}"
