# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause

GEM5_ROOT ?= $(abspath ../../..)
GEM5_BIN  ?= $(GEM5_ROOT)/build/RISCV/gem5.opt
DRIVER    := $(abspath driver/rbook_testbench_gem5_v2.py)
RUN       := $(GEM5_ROOT)/util/run_with_timeout.sh

TIMESTAMP  = $(shell date +%Y%m%d-%H%M%S)

# Per-tile CHI request-node mode forwarded to --rn-mode. The tile
# controller's attachment is identical to v1; the driver (separate
# CHI node on the same router) is always the same regardless of mode.
#   rnf_l2 -> tile acts as a coherent L2 leaf cache
#   rni    -> tile is a cache-less pass-through (subset of scenarios)
RN_MODE ?= rnf_l2

# Scenarios added incrementally as the driver capability lands. The
# full v2 set mirrors v1 plus the new atomic_rmw.
SCENARIOS := idle smoke_read smoke_write smoke_opcode_mix \
             memcpy memset ping_pong false_sharing \
             opcode_walk read_ex_walk atomic_rmw

.PHONY: all help run-all clean
.PHONY: $(addprefix run-,$(SCENARIOS))

all: help

help:
	@echo "chi_testbench_gem5_v2 — CPU-less CHI testbench with direct"
	@echo "CHI message injection (Driver acts as an upstream RN sharing the"
	@echo "tile's mesh router)."
	@echo
	@echo "Scenarios (pass RN_MODE=rni or RN_MODE=rnf_l2; default rnf_l2):"
	@for s in $(SCENARIOS); do echo "  run-$$s"; done
	@echo
	@echo "Meta:"
	@echo "  run-all          run every scenario sequentially"
	@echo "  clean            remove local m5out-link artifacts"
	@echo
	@echo "Output lands in m5out/last-<scenario>-<RN_MODE>/ (symlink)."

run-all: $(addprefix run-,$(SCENARIOS))

define RUN_SCENARIO_RULE
run-$(1):
	@mkdir -p m5out
	$$(RUN) $$(GEM5_BIN) \
	    -d $$(GEM5_ROOT)/m5out/rbook-tb-gem5-v2-$(1)-$$(RN_MODE)-$$(TIMESTAMP) \
	    $$(DRIVER) \
	    --scenario=$(1) \
	    --rn-mode=$$(RN_MODE)
	@ln -sfn $$(GEM5_ROOT)/m5out/rbook-tb-gem5-v2-$(1)-$$(RN_MODE)-$$(TIMESTAMP) \
	    m5out/last-$(1)-$$(RN_MODE)
endef

$(foreach s,$(SCENARIOS),$(eval $(call RUN_SCENARIO_RULE,$(s))))

clean:
	rm -rf m5out
