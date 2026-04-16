SHELL = bash

CC = riscv64-linux-gnu-gcc
CFLAGS = -O2 -static
REPO_ROOT = $(abspath ../../..)
GEM5_INCLUDE = $(REPO_ROOT)/include
M5OPS_RISCV = $(REPO_ROOT)/util/m5/src/abi/riscv/m5op.S
GEM5_BIN = $(REPO_ROOT)/build/RISCV/gem5.opt
RUN_WITH_TIMEOUT = $(REPO_ROOT)/util/run_with_timeout.sh
GEM5_CONFIG = $(REPO_ROOT)/configs/example/rbook_mesh_config.py
M5OUT_ROOT = $(REPO_ROOT)/m5out
BIN = rbook_test_link_pressure
BIN_PATH = $(abspath $(BIN))
LAST_OUTDIR = .last_outdir
REPORT = ValidatedRunReport.md
PAGES ?= 1536
MEM_SIZE ?= 512MiB
PER_VNET_LINKS ?=
RUN_TAG ?= rbook-link-pressure

all: report

build: $(BIN)

$(BIN): rbook_test_link_pressure.c
	@$(CC) $(CFLAGS) -I$(GEM5_INCLUDE) -o $@ $< $(M5OPS_RISCV) -lpthread

run: $(BIN)
	@set -euo pipefail; \
	outdir="$(M5OUT_ROOT)/$(RUN_TAG)-$$(date +%Y%m%d-%H%M%S)"; \
	mesh_arg=(); \
	if [[ -n "$(PER_VNET_LINKS)" ]]; then \
	    mesh_arg+=(--per-vnet-links); \
	fi; \
	mkdir "$$outdir"; \
	printf 'Starting link-pressure simulation: %s (pages=%s mem=%s per_vnet=%s)\n' \
	    "$$outdir" "$(PAGES)" "$(MEM_SIZE)" "$${PER_VNET_LINKS:-off}"; \
	"$(RUN_WITH_TIMEOUT)" "$(GEM5_BIN)" -d "$$outdir" "$(GEM5_CONFIG)" \
	    --mem-size="$(MEM_SIZE)" \
	    --cmd="$(BIN_PATH)" \
	    --options="$(PAGES)" \
	    "$${mesh_arg[@]}" \
	    > "$$outdir/console.log" 2>&1; \
	printf '%s\n' "$$outdir" > "$(LAST_OUTDIR)"; \
	printf 'Run directory: %s\n' "$$outdir"

check: run
	@set -euo pipefail; \
	outdir="$$(<"$(LAST_OUTDIR)")"; \
	python3 check_link_pressure.py "$$outdir" "$$outdir/console.log"

report: check
	@set -euo pipefail; \
	outdir="$$(<"$(LAST_OUTDIR)")"; \
	python3 report_link_pressure.py "$$outdir" "$$outdir/console.log" "$(REPORT)"; \
	printf 'Report: %s\n' "$(abspath $(REPORT))"

run_shared_links:
	@$(MAKE) report PER_VNET_LINKS= RUN_TAG=rbook-link-pressure-shared

run_per_vnet_links:
	@$(MAKE) report PER_VNET_LINKS=1 RUN_TAG=rbook-link-pressure-pervnet

compare:
	@$(MAKE) run_shared_links
	@$(MAKE) run_per_vnet_links

clean:
	@rm -f $(BIN) $(LAST_OUTDIR) $(REPORT)
	@rm -rf __pycache__

.PHONY: all build run check report clean run_shared_links run_per_vnet_links compare
