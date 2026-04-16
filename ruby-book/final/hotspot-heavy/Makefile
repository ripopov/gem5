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
BIN = rbook_test_hotspot_heavy
BIN_PATH = $(abspath $(BIN))
LAST_OUTDIR = .last_outdir
REPORT = ValidatedRunReport.md
PAGES ?= 2048
READ_STREAMS ?= 1
WRITE_STREAMS ?= 0
MEM_SIZE ?= 512MiB
CPU_LQ_ENTRIES ?=
CPU_SQ_ENTRIES ?=
CPU_ROB_ENTRIES ?=
CPU_IQ_ENTRIES ?=
L1D_SEQ_OUTSTANDING ?=
L1D_TBES ?=
L2_TBES ?=
HNF_TBES ?=
TRACE_OUTPUT ?=
TRACE_FORMAT ?= ftr
WAVE_TRACE ?=
PER_VNET_LINKS ?=

all: report

build: $(BIN)

$(BIN): rbook_test_hotspot_heavy.c
	@$(CC) $(CFLAGS) -I$(GEM5_INCLUDE) -o $@ $< $(M5OPS_RISCV) -lpthread

run: $(BIN)
	@set -euo pipefail; \
	outdir="$(M5OUT_ROOT)/rbook-hotspot-heavy-$$(date +%Y%m%d-%H%M%S)"; \
	trace_arg=(); \
	wave_arg=(); \
	mesh_arg=(); \
	cpu_arg=(); \
	trace_path=""; \
	if [[ -n "$(WAVE_TRACE)" ]]; then \
	    wave_arg+=(--wave-trace); \
	fi; \
	if [[ -n "$(PER_VNET_LINKS)" ]]; then \
	    mesh_arg+=(--per-vnet-links); \
	fi; \
	if [[ -n "$(CPU_LQ_ENTRIES)" ]]; then \
	    cpu_arg+=(--cpu-lq-entries="$(CPU_LQ_ENTRIES)"); \
	fi; \
	if [[ -n "$(CPU_SQ_ENTRIES)" ]]; then \
	    cpu_arg+=(--cpu-sq-entries="$(CPU_SQ_ENTRIES)"); \
	fi; \
	if [[ -n "$(CPU_ROB_ENTRIES)" ]]; then \
	    cpu_arg+=(--cpu-rob-entries="$(CPU_ROB_ENTRIES)"); \
	fi; \
	if [[ -n "$(CPU_IQ_ENTRIES)" ]]; then \
	    cpu_arg+=(--cpu-iq-entries="$(CPU_IQ_ENTRIES)"); \
	fi; \
	if [[ -n "$(L1D_SEQ_OUTSTANDING)" ]]; then \
	    cpu_arg+=(--l1d-seq-outstanding="$(L1D_SEQ_OUTSTANDING)"); \
	fi; \
	if [[ -n "$(L1D_TBES)" ]]; then \
	    cpu_arg+=(--l1d-tbes="$(L1D_TBES)"); \
	fi; \
	if [[ -n "$(L2_TBES)" ]]; then \
	    cpu_arg+=(--l2-tbes="$(L2_TBES)"); \
	fi; \
	if [[ -n "$(HNF_TBES)" ]]; then \
	    cpu_arg+=(--hnf-tbes="$(HNF_TBES)"); \
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
	printf 'Starting hotspot-heavy simulation: %s (pages=%s read=%s write=%s mem=%s)\n' \
	    "$$outdir" "$(PAGES)" "$(READ_STREAMS)" "$(WRITE_STREAMS)" "$(MEM_SIZE)"; \
	"$(RUN_WITH_TIMEOUT)" "$(GEM5_BIN)" -d "$$outdir" "$(GEM5_CONFIG)" \
	    --mem-size="$(MEM_SIZE)" \
	    --cmd="$(BIN_PATH)" \
	    --options="$(PAGES) $(READ_STREAMS) $(WRITE_STREAMS)" \
	    "$${mesh_arg[@]}" \
	    "$${wave_arg[@]}" \
	    "$${cpu_arg[@]}" \
	    "$${trace_arg[@]}" \
	    > "$$outdir/console.log" 2>&1; \
	printf '%s\n' "$$outdir" > "$(LAST_OUTDIR)"; \
	printf 'Run directory: %s\n' "$$outdir"; \
	if [[ -n "$$trace_path" ]]; then \
	    printf 'Trace file: %s\n' "$$trace_path"; \
	fi

check: run
	@set -euo pipefail; \
	outdir="$$(<"$(LAST_OUTDIR)")"; \
	python3 check_hotspot_heavy.py "$$outdir" "$$outdir/console.log"

report: check
	@set -euo pipefail; \
	outdir="$$(<"$(LAST_OUTDIR)")"; \
	python3 report_hotspot_heavy.py "$$outdir" "$$outdir/console.log" "$(REPORT)"; \
	printf 'Report: %s\n' "$(abspath $(REPORT))"

clean:
	@rm -f $(BIN) $(LAST_OUTDIR) $(REPORT)
	@rm -rf __pycache__

.PHONY: all build run check report clean
