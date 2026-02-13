SHELL := /bin/bash

OUTDIR ?= ./out/riscv-hello-o3
GEM5_BIN := build/RISCV/gem5.opt
CONFIG_SCRIPT := tests/gem5/stdlib/configs/simple_binary_run.py
TRACE_FILE := $(OUTDIR)/trace.out
PIPEVIEW_FILE := $(OUTDIR)/o3-pipeview.out

.PHONY: riscv-hello
riscv-hello:
	@if [[ "$(OUTDIR)" != ./out/* ]]; then \
		echo "error: OUTDIR must start with ./out/" >&2; \
		exit 1; \
	fi
	@mkdir -p "$(OUTDIR)"
	@if [[ ! -x "$(GEM5_BIN)" ]]; then \
		echo "gem5 binary not found at $(GEM5_BIN); building it..."; \
		scons build/RISCV/gem5.opt; \
	fi
	@"$(GEM5_BIN)" -d "$(OUTDIR)" \
		--debug-flags=O3PipeView \
		--debug-file=trace.out \
		"$(CONFIG_SCRIPT)" \
		riscv-hello o3 riscv
	@./util/o3-pipeview.py -c 333 \
		-o "$(PIPEVIEW_FILE)" \
		"$(TRACE_FILE)"
	@echo "Raw trace: $(TRACE_FILE)"
	@echo "Rendered timeline: $(PIPEVIEW_FILE)"
