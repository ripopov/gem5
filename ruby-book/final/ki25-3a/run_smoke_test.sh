#!/bin/bash
# run_smoke_test.sh - Run the smoke test simulation
#
# Usage: ./run_smoke_test.sh [output_dir_suffix]
# Example: ./run_smoke_test.sh test1

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
GEM5_BIN="${REPO_ROOT}/build/RISCV/gem5.opt"
CONFIG_SCRIPT="${REPO_ROOT}/configs/example/rbook_mesh_config.py"
TEST_BINARY="${SCRIPT_DIR}/rbook_test_smoke"

# Check if gem5 is built
if [ ! -f "${GEM5_BIN}" ]; then
    echo "ERROR: gem5 binary not found at ${GEM5_BIN}"
    echo "Please build gem5 with: scons build/RISCV/gem5.opt PROTOCOL=CHI"
    exit 1
fi

# Check if test binary exists
if [ ! -f "${TEST_BINARY}" ]; then
    echo "ERROR: Test binary not found at ${TEST_BINARY}"
    echo "Please build with: make"
    exit 1
fi

# Create output directory
TIMESTAMP=$(date +%Y%m%d-%H%M%S)
SUFFIX="${1:-${TIMESTAMP}}"
OUTPUT_DIR="${SCRIPT_DIR}/m5out-smoke-${SUFFIX}"

echo "=========================================="
echo "Running KI25-3A Smoke Test"
echo "=========================================="
echo "Output directory: ${OUTPUT_DIR}"
echo "Test binary: ${TEST_BINARY}"
echo ""

# Run simulation
"${GEM5_BIN}" \
    -d "${OUTPUT_DIR}" \
    "${CONFIG_SCRIPT}" \
    --cmd="${TEST_BINARY}" \
    2>&1 | tee "${SCRIPT_DIR}/simulation_${SUFFIX}.log"

echo ""
echo "=========================================="
echo "Simulation Complete"
echo "=========================================="
echo "Output: ${OUTPUT_DIR}"
echo ""
echo "To analyze results, run:"
echo "  cd ${SCRIPT_DIR}"
echo "  python3 ki25-3a_smoke_test_analysis.py ${OUTPUT_DIR}/stats.txt"
