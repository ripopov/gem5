#!/usr/bin/env bash

set -euo pipefail

script_dir=$(readlink -f "$(dirname "$0")")
repo_root=$(readlink -f "$script_dir/../../..")
run_dir=${1:-"$script_dir/m5out-smoke-$(date +%Y%m%d-%H%M%S)"}
report_path="$script_dir/summary_report.md"
console_log="$run_dir/console.log"

make -C "$script_dir"

mkdir -p "$run_dir"

"$repo_root/build/RISCV/gem5.opt" \
    -d "$run_dir" \
    "$repo_root/configs/example/rbook_mesh_config.py" \
    --cmd="$script_dir/rbook_test_smoke" \
    --l3_size=512KiB 2>&1 | tee "$console_log"

python3 "$script_dir/analyze_smoke_stats.py" \
    --run-dir "$run_dir" \
    --out "$report_path"

printf 'Report written to %s\n' "$report_path"
