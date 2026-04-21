# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause

GEM5_ROOT ?= $(abspath ../../..)
GEM5_BIN  ?= $(GEM5_ROOT)/build/RISCV/gem5.opt
DRIVER    := $(abspath driver/rbook_testbench.py)
RUN       := $(GEM5_ROOT)/util/run_with_timeout.sh

TIMESTAMP  = $(shell date +%Y%m%d-%H%M%S)

SCENARIOS := smoke_read smoke_write smoke_opcode_mix

.PHONY: all help run-all clean
.PHONY: $(addprefix run-,$(SCENARIOS))

all: help

help:
	@echo "chi_testbench — CPU-less SystemC CHI-Garnet testbench"
	@echo
	@echo "Scenarios:"
	@for s in $(SCENARIOS); do echo "  run-$$s"; done
	@echo
	@echo "Meta:"
	@echo "  run-all          run every scenario sequentially"
	@echo "  clean            remove local m5out-link artifacts"

run-all: $(addprefix run-,$(SCENARIOS))

# Explicit per-scenario rule body (phony targets + pattern rules with
# empty prerequisites do not compose reliably in GNU make).
define RUN_SCENARIO_RULE
run-$(1):
	@mkdir -p m5out
	$$(RUN) $$(GEM5_BIN) \
	    -d $$(GEM5_ROOT)/m5out/rbook-tb-$(1)-$$(TIMESTAMP) \
	    $$(DRIVER) \
	    --scenario=$(1)
	@ln -sfn $$(GEM5_ROOT)/m5out/rbook-tb-$(1)-$$(TIMESTAMP) \
	    m5out/last-$(1)
endef

$(foreach s,$(SCENARIOS),$(eval $(call RUN_SCENARIO_RULE,$(s))))

clean:
	rm -rf m5out
