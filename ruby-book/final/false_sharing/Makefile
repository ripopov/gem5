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
BIN = rbook_test_false_sharing
PTHREAD_PROBE_BIN = rbook_probe_pthreads
BIN_PATH = $(abspath $(BIN))
PTHREAD_PROBE_BIN_PATH = $(abspath $(PTHREAD_PROBE_BIN))
LAST_OUTDIR = .last_outdir
REPORT = ValidatedRunReport.md
ITERATIONS ?= 128
MONITOR = monitor_false_sharing.py
TRACE_OUTPUT ?=
TRACE_FORMAT ?= ftr

all: report

build: $(BIN)

probe_build: $(PTHREAD_PROBE_BIN)

$(BIN): rbook_test_false_sharing.c
	@$(CC) $(CFLAGS) -I$(GEM5_INCLUDE) -o $@ $< $(M5OPS_RISCV) -lpthread

$(PTHREAD_PROBE_BIN): rbook_probe_pthreads.c
	@$(CC) $(CFLAGS) -I$(GEM5_INCLUDE) -o $@ $< $(M5OPS_RISCV) -lpthread

probe_run: $(PTHREAD_PROBE_BIN)
	@set -euo pipefail; \
	outdir="$(M5OUT_ROOT)/rbook-pthread-probe-$$(date +%Y%m%d-%H%M%S)"; \
	trace_arg=(); \
	trace_path=""; \
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
	printf 'Starting pthread probe: %s\n' "$$outdir"; \
	"$(RUN_WITH_TIMEOUT)" "$(GEM5_BIN)" -d "$$outdir" "$(GEM5_CONFIG)" \
	    --cmd="$(PTHREAD_PROBE_BIN_PATH)" "$${trace_arg[@]}" \
	    > "$$outdir/console.log" 2>&1; \
	printf '%s\n' "$$outdir" > "$(LAST_OUTDIR)"; \
	printf 'Run directory: %s\n' "$$outdir"; \
	if [[ -n "$$trace_path" ]]; then \
	    printf 'Trace file: %s\n' "$$trace_path"; \
	fi

run: $(BIN) $(MONITOR)
	@set -euo pipefail; \
	outdir="$(M5OUT_ROOT)/rbook-false-sharing-$$(date +%Y%m%d-%H%M%S)"; \
	trace_arg=(); \
	trace_path=""; \
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
	printf 'Starting false-sharing simulation: %s (iterations=%s)\n' \
	    "$$outdir" "$(ITERATIONS)"; \
	"$(RUN_WITH_TIMEOUT)" "$(GEM5_BIN)" -d "$$outdir" "$(GEM5_CONFIG)" \
	    --cmd="$(BIN_PATH)" --options="$(ITERATIONS)" \
	    "$${trace_arg[@]}" \
	    > "$$outdir/console.log" 2>&1 & \
	gem5_pid=$$!; \
	python3 "$(MONITOR)" "$$gem5_pid" "$$outdir/console.log"; \
	wait "$$gem5_pid"; \
	printf '%s\n' "$$outdir" > "$(LAST_OUTDIR)"; \
	printf 'Run directory: %s\n' "$$outdir"; \
	if [[ -n "$$trace_path" ]]; then \
	    printf 'Trace file: %s\n' "$$trace_path"; \
	fi

check: run
	@set -euo pipefail; \
	outdir="$$(<"$(LAST_OUTDIR)")"; \
	python3 check_false_sharing.py "$$outdir" "$$outdir/console.log"

report: check
	@set -euo pipefail; \
	outdir="$$(<"$(LAST_OUTDIR)")"; \
	python3 report_false_sharing.py "$$outdir" "$$outdir/console.log" "$(REPORT)"; \
	printf 'Report: %s\n' "$(abspath $(REPORT))"

clean:
	@rm -f $(BIN) $(PTHREAD_PROBE_BIN) $(LAST_OUTDIR) $(REPORT)
	@rm -rf __pycache__

.PHONY: all build probe_build run probe_run check report clean
