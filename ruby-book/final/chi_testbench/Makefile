# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause

GEM5_ROOT ?= $(abspath ../../..)
GEM5_BIN  ?= $(GEM5_ROOT)/build/RISCV/gem5.opt
DRIVER    := $(abspath driver/rbook_testbench.py)
RUN       := $(GEM5_ROOT)/util/run_with_timeout.sh

TIMESTAMP  = $(shell date +%Y%m%d-%H%M%S)

SCENARIOS := smoke_read

.PHONY: all help $(addprefix run-,$(SCENARIOS)) clean

all: help

help:
	@echo "chi_testbench — CPU-less SystemC CHI-Garnet testbench"
	@echo
	@echo "Targets:"
	@echo "  run-smoke_read   run Stage-1 smoke read scenario"
	@echo "  clean            remove local m5out-link artifacts"

run-smoke_read:
	@mkdir -p m5out
	$(RUN) $(GEM5_BIN) \
	    -d $(GEM5_ROOT)/m5out/rbook-tb-smoke_read-$(TIMESTAMP) \
	    $(DRIVER) \
	    --scenario=smoke_read
	@ln -sfn $(GEM5_ROOT)/m5out/rbook-tb-smoke_read-$(TIMESTAMP) \
	    m5out/last-smoke_read

clean:
	rm -rf m5out
