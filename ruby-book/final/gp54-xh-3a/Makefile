GEM5_ROOT := $(abspath ../../..)
CC := riscv64-linux-gnu-gcc
CFLAGS := -O2 -static -std=c11 -Wall -Wextra \
	-D_POSIX_C_SOURCE=200112L -I $(GEM5_ROOT)/include
SMOKE_SLICE_BYTES ?= 524288
SMOKE_PASSES ?= 2
M5OPS_SRC := $(GEM5_ROOT)/util/m5/src/abi/riscv/m5op.S
BIN := rbook_test_smoke

all: $(BIN)

$(BIN): rbook_test_smoke.c
	$(CC) $(CFLAGS) \
	    -DRBOOK_L3_SLICE_BYTES=$(SMOKE_SLICE_BYTES) \
	    -DRBOOK_SWEEP_PASSES=$(SMOKE_PASSES) \
	    -o $@ $(M5OPS_SRC) $<

clean:
	rm -f $(BIN)

.PHONY: all clean
