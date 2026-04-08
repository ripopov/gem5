#!/bin/bash
# run_smoke.sh — Build, run, and analyze the single-core smoke test.
#
# Usage: ./run_smoke.sh
#
# The script:
#   1. Cross-compiles rbook_test_smoke.c for RISC-V
#   2. Runs the simulation on the 4x4 CHI mesh
#   3. Runs the analysis script on the results
#   4. Copies the report back to this directory

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GEM5_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
GEM5_BIN="${GEM5_ROOT}/build/RISCV/gem5.opt"
CONFIG="${GEM5_ROOT}/configs/example/rbook_mesh_config.py"
BINARY="${SCRIPT_DIR}/rbook_test_smoke"
ANALYZE="${SCRIPT_DIR}/analyze_smoke.py"
CC="riscv64-linux-gnu-gcc"

echo "=== Step 1: Cross-compile ==="
${CC} -O2 -static -o "${BINARY}" "${SCRIPT_DIR}/rbook_test_smoke.c"
echo "  Built: ${BINARY}"

echo "=== Step 2: Run simulation ==="
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
OUTDIR="${GEM5_ROOT}/m5out/rbook-smoke-${TIMESTAMP}"
mkdir -p "${OUTDIR}"
${GEM5_BIN} -d "${OUTDIR}" "${CONFIG}" --cmd="${BINARY}"
echo "  Output: ${OUTDIR}"

echo "=== Step 3: Analyze ==="
python3 "${ANALYZE}" "${OUTDIR}"
echo ""

echo "=== Step 4: Copy report ==="
if [ -f "${OUTDIR}/smoke_report.txt" ]; then
    cp "${OUTDIR}/smoke_report.txt" "${SCRIPT_DIR}/smoke_report.txt"
    echo "  Report: ${SCRIPT_DIR}/smoke_report.txt"
else
    echo "  WARNING: smoke_report.txt not found in output dir"
fi

echo "=== Done ==="
