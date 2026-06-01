#!/usr/bin/env bash
#
# Run the CHI testbench "memset" sequence
# (src/chi_testbench_gem5/sequences/memset.cc).
#
# Output lands in a timestamped m5out/ directory; the
# ruby-book/final/chi_testbench_gem5/m5out/last-memset-<mode> symlink
# is updated to point at it.
#
# Tunables (override via environment):
#   RN_MODE=rnf_l2|rni        per-tile request-node mode (default rnf_l2)
#   ACTIVE_CORES=all|0,1,...  which tiles run the workload (default all)
#   NUM_OUTSTANDING_REQS=N    request pipeline depth (default 4)
#
# Usage: ./runtest.sh
#        RN_MODE=rni ./runtest.sh

set -euo pipefail

cd "$(dirname "$0")"

GEM5_BIN=build/RISCV/gem5.opt
DRIVER=ruby-book/final/chi_testbench_gem5/driver/rbook_testbench_gem5.py
TB_DIR=ruby-book/final/chi_testbench_gem5

if [[ ! -x "${GEM5_BIN}" ]]; then
    echo "${GEM5_BIN} not found -- run ./build.sh first" >&2
    exit 1
fi

OUTDIR="m5out/rbook-tb-gem5-memset"

mkdir -p "${TB_DIR}/m5out"

# --per-vnet-links

"${GEM5_BIN}" \
    -d "${OUTDIR}" \
    "${DRIVER}" \
    --scenario=memset \
    --active-cores=0 \
    --network=simple --simple-physical-channels \
    --num-outstanding-reqs=32
