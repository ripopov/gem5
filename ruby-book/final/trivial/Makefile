SHELL = bash

CC = riscv64-linux-gnu-gcc
CFLAGS = -O2 -static
REPO_ROOT = $(abspath ../../..)
GEM5_BIN = $(REPO_ROOT)/build/RISCV/gem5.opt
RUN_WITH_TIMEOUT = $(REPO_ROOT)/util/run_with_timeout.sh
GEM5_CONFIG = $(REPO_ROOT)/configs/example/rbook_mesh_config.py
M5OUT_ROOT = $(REPO_ROOT)/m5out
BIN = trivial
BIN_PATH = $(abspath $(BIN))
LAST_OUTDIR = .last_outdir

all: build

build: $(BIN)

$(BIN): trivial.c
	$(CC) $(CFLAGS) -o $@ $<

run: build
	@set -euo pipefail; \
	outdir="$(M5OUT_ROOT)/rbook-topology-$$(date +%Y%m%d-%H%M%S)"; \
	mkdir "$$outdir"; \
	"$(RUN_WITH_TIMEOUT)" "$(GEM5_BIN)" -d "$$outdir" "$(GEM5_CONFIG)" \
	    --cmd="$(BIN_PATH)" > "$$outdir/console.log" 2>&1; \
	printf '%s\n' "$$outdir" > "$(LAST_OUTDIR)"; \
	printf 'Run directory: %s\n' "$$outdir"

clean:
	rm -f $(BIN) $(LAST_OUTDIR)

.PHONY: all build run clean
