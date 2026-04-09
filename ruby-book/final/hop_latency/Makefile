SHELL = bash

CC = riscv64-linux-gnu-gcc
CFLAGS = -O2 -static
REPO_ROOT = $(abspath ../../..)
GEM5_INCLUDE = $(REPO_ROOT)/include
M5OPS_RISCV = $(REPO_ROOT)/util/m5/src/abi/riscv/m5op.S
GEM5_BIN = $(REPO_ROOT)/build/RISCV/gem5.opt
GEM5_CONFIG = $(REPO_ROOT)/configs/example/rbook_mesh_config.py
M5OUT_ROOT = $(REPO_ROOT)/m5out
BIN = rbook_test_hop_latency
BIN_PATH = $(abspath $(BIN))
LAST_OUTDIR = .last_outdir
REPORT = ValidatedRunReport.md

all: report

build: $(BIN)

$(BIN): rbook_test_hop_latency.c
	@$(CC) $(CFLAGS) -I$(GEM5_INCLUDE) -o $@ $< $(M5OPS_RISCV)

run: $(BIN)
	@set -euo pipefail; \
	outdir="$(M5OUT_ROOT)/rbook-hop-latency-$$(date +%Y%m%d-%H%M%S)"; \
	mkdir "$$outdir"; \
	"$(GEM5_BIN)" -d "$$outdir" "$(GEM5_CONFIG)" \
	    --cmd="$(BIN_PATH)" > "$$outdir/console.log" 2>&1; \
	printf '%s\n' "$$outdir" > "$(LAST_OUTDIR)"; \
	printf 'Run directory: %s\n' "$$outdir"

check: run
	@set -euo pipefail; \
	outdir="$$(<"$(LAST_OUTDIR)")"; \
	python3 check_hop_latency.py "$$outdir" "$$outdir/console.log"

report: check
	@set -euo pipefail; \
	outdir="$$(<"$(LAST_OUTDIR)")"; \
	python3 report_hop_latency.py "$$outdir" "$$outdir/console.log" "$(REPORT)"; \
	printf 'Report: %s\n' "$(abspath $(REPORT))"

clean:
	@rm -f $(BIN) $(LAST_OUTDIR)

.PHONY: all build run check report clean
