# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause

# Builds the syscall-emulation memory workload used to check CPU models
# that access guest memory directly.  Statically linked, because gem5's
# syscall emulation has no dynamic loader for this target by default.

CROSS_COMPILE ?= riscv64-linux-gnu-
CC = $(CROSS_COMPILE)gcc
STRIP = $(CROSS_COMPILE)strip
CFLAGS ?= -O2 -static -Wall
OUT = se-memory
BIN = ../bin/riscv/linux/$(OUT)

.PHONY: all install clean

all: $(OUT)

$(OUT): se-memory.c
	$(CC) $(CFLAGS) -o $@ $<

# The checked-in copy is stripped: it stays under the repository's
# added-file size limit and the symbols play no part in the test.
install: $(OUT)
	mkdir -p $(dir $(BIN))
	$(STRIP) -o $(BIN) $(OUT)

clean:
	rm -f $(OUT)
