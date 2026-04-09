SHELL = bash

CC = riscv64-linux-gnu-gcc
CFLAGS = -O2 -static
REPO_ROOT = $(abspath ../../..)
GEM5_BIN = $(REPO_ROOT)/build/RISCV/gem5.opt
GEM5_CONFIG = $(REPO_ROOT)/configs/example/rbook_mesh_config.py
M5OUT_ROOT = $(REPO_ROOT)/m5out
BIN = rbook_test_smoke
BIN_PATH = $(abspath $(BIN))
LAST_OUTDIR = .last_outdir
REPORT = ValidatedRunReport.md

all: build

build: $(BIN)

$(BIN): rbook_test_smoke.c
	$(CC) $(CFLAGS) -o $@ $<

run: build
	@set -euo pipefail; \
	outdir="$(M5OUT_ROOT)/rbook-smoke-$$(date +%Y%m%d-%H%M%S)"; \
	mkdir "$$outdir"; \
	printf 'Starting smoke simulation: %s\n' "$$outdir"; \
	"$(GEM5_BIN)" -d "$$outdir" "$(GEM5_CONFIG)" \
	    --cmd="$(BIN_PATH)" > "$$outdir/console.log" 2>&1; \
	printf '%s\n' "$$outdir" > "$(LAST_OUTDIR)"; \
	printf 'Run directory: %s\n' "$$outdir"

check: run
	@set -euo pipefail; \
	outdir="$$(<"$(LAST_OUTDIR)")"; \
	python3 check_smoke.py "$$outdir" "$$outdir/console.log"

report: check
	@set -euo pipefail; \
	outdir="$$(<"$(LAST_OUTDIR)")"; \
	python3 report_smoke.py "$$outdir" "$$outdir/console.log" "$(REPORT)"; \
	printf 'Report: %s\n' "$(abspath $(REPORT))"

clean:
	rm -f $(BIN) $(LAST_OUTDIR) $(REPORT)
	rm -rf __pycache__

.PHONY: all build run check report clean
