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
BIN = rbook_test_hotspot
BIN_PATH = $(abspath $(BIN))
LAST_OUTDIR = .last_outdir
REPORT = ValidatedRunReport.md
PAGES ?= 2048
TRACE_OUTPUT ?=
TRACE_FORMAT ?= ftr
WAVE_TRACE ?=
PER_VNET_LINKS ?=

all: report

build: $(BIN)

$(BIN): rbook_test_hotspot.c
	@$(CC) $(CFLAGS) -I$(GEM5_INCLUDE) -o $@ $< $(M5OPS_RISCV) -lpthread

run: run_shared_links

run_shared_links: $(BIN)
	@set -euo pipefail; \
	outdir="$(M5OUT_ROOT)/rbook-hotspot-$$(date +%Y%m%d-%H%M%S)"; \
	trace_arg=(); \
	wave_arg=(); \
	mesh_arg=(); \
	trace_path=""; \
	if [[ -n "$(WAVE_TRACE)" ]]; then \
	    wave_arg+=(--wave-trace); \
	fi; \
	if [[ -n "$(PER_VNET_LINKS)" ]]; then \
	    mesh_arg+=(--per-vnet-links); \
	fi; \
	if [[ -n "$(TRACE_OUTPUT)" ]]; then \
	    trace_arg+=(--trace-output="$(TRACE_OUTPUT)"); \
	    trace_arg+=(--trace-format="$(TRACE_FORMAT)"); \
	    trace_ext=".ftr"; \
	    if [[ "$(TRACE_FORMAT)" == "text" ]]; then \
	        trace_ext=".txlog"; \
	    fi; \
	    if [[ "$(TRACE_OUTPUT)" = /* ]]; then \
	        trace_path="$(TRACE_OUTPUT)$$trace_ext"; \
	    else \
	        trace_path="$$outdir/$(TRACE_OUTPUT)$$trace_ext"; \
	    fi; \
	fi; \
	mkdir "$$outdir"; \
	printf 'Starting hotspot simulation: %s (pages=%s)\n' \
	    "$$outdir" "$(PAGES)"; \
	"$(RUN_WITH_TIMEOUT)" "$(GEM5_BIN)" -d "$$outdir" "$(GEM5_CONFIG)" \
	    --cmd="$(BIN_PATH)" --options="$(PAGES)" \
	    "$${mesh_arg[@]}" \
	    "$${wave_arg[@]}" \
	    "$${trace_arg[@]}" \
	    > "$$outdir/console.log" 2>&1; \
	printf '%s\n' "$$outdir" > "$(LAST_OUTDIR)"; \
	printf 'Run directory: %s\n' "$$outdir"; \
	if [[ -n "$$trace_path" ]]; then \
	    printf 'Trace file: %s\n' "$$trace_path"; \
	fi

run_per_vnet_links: $(BIN)
	@$(MAKE) run_shared_links PER_VNET_LINKS=1

check: run
	@set -euo pipefail; \
	outdir="$$(<"$(LAST_OUTDIR)")"; \
	python3 check_hotspot.py "$$outdir" "$$outdir/console.log"

report: check
	@set -euo pipefail; \
	outdir="$$(<"$(LAST_OUTDIR)")"; \
	python3 report_hotspot.py "$$outdir" "$$outdir/console.log" "$(REPORT)"; \
	printf 'Report: %s\n' "$(abspath $(REPORT))"

clean:
	@rm -f $(BIN) $(LAST_OUTDIR) $(REPORT)
	@rm -rf __pycache__

.PHONY: all build run run_shared_links run_per_vnet_links check report clean
