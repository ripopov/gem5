# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause

GEM5_ROOT ?= $(abspath ../../..)
GEM5_BIN  ?= $(GEM5_ROOT)/build/RISCV/gem5.opt
DRIVER    := $(abspath driver/rbook_testbench_gem5.py)
RUN       := $(GEM5_ROOT)/util/run_with_timeout.sh

TIMESTAMP  = $(shell date +%Y%m%d-%H%M%S)

# Per-tile CHI request-node mode forwarded to --rn-mode.
# Valid values: rnf_l2 (Seq -> coherent L2 leaf cache -> mesh, default)
#               rni    (Seq -> cache-less DMA ctrl  -> mesh)
RN_MODE ?= rnf_l2
ACTIVE_CORES ?= all
NUM_OUTSTANDING_REQS ?= 4
NETWORK ?= simple
MESSAGE_BUFFER_VCD ?= 0
EXTRA_ARGS ?=

ifeq ($(MESSAGE_BUFFER_VCD),1)
MESSAGE_BUFFER_VCD_ARGS := --message-buffer-vcd
endif

SCENARIOS := ping_pong memset

.PHONY: all help run-all clean
.PHONY: $(addprefix run-,$(SCENARIOS))

all: help

help:
	@echo "chi_testbench_gem5 — CPU-less CHI testbench (pure gem5 APIs)"
	@echo
	@echo "Scenarios (pass RN_MODE=rni or RN_MODE=rnf_l2; default rnf_l2):"
	@for s in $(SCENARIOS); do echo "  run-$$s"; done
	@echo "Networks: pass NETWORK=simple, NETWORK=simple_xp, or NETWORK=garnet."
	@echo
	@echo "Meta:"
	@echo "  run-all          run every scenario sequentially"
	@echo "  clean            remove local m5out-link artifacts"
	@echo
	@echo "Output lands in m5out/last-<scenario>-<RN_MODE>/ (symlink)."

run-all: $(addprefix run-,$(SCENARIOS))

# Explicit per-scenario rule body (phony targets + pattern rules with
# empty prerequisites do not compose reliably in GNU make).
define RUN_SCENARIO_RULE
run-$(1):
	@mkdir -p m5out
	$$(RUN) $$(GEM5_BIN) \
	    -d $$(GEM5_ROOT)/m5out/rbook-tb-gem5-$(1)-$$(RN_MODE)-$$(TIMESTAMP) \
	    $$(DRIVER) \
	    --scenario=$(1) \
	    --network=$$(NETWORK) \
	    --active-cores=$$(ACTIVE_CORES) \
	    --num-outstanding-reqs=$$(NUM_OUTSTANDING_REQS) \
	    --rn-mode=$$(RN_MODE) \
	    $$(MESSAGE_BUFFER_VCD_ARGS) \
	    $$(EXTRA_ARGS)
	@ln -sfn $$(GEM5_ROOT)/m5out/rbook-tb-gem5-$(1)-$$(RN_MODE)-$$(TIMESTAMP) \
	    m5out/last-$(1)-$$(RN_MODE)
endef

$(foreach s,$(SCENARIOS),$(eval $(call RUN_SCENARIO_RULE,$(s))))

clean:
	rm -rf m5out
