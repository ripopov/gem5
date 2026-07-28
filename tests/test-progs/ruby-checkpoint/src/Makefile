# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause

CROSS_COMPILE ?= riscv64-linux-gnu-
CC = $(CROSS_COMPILE)gcc
CFLAGS = -O1 -march=rv64gc -mabi=lp64d -static

.PHONY: all clean

all: ruby-checkpoint

ruby-checkpoint: ruby-checkpoint.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f ruby-checkpoint
