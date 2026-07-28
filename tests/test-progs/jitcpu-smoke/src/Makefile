# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause

CROSS_COMPILE ?= riscv64-linux-gnu-
CC = $(CROSS_COMPILE)gcc
DINING_CC ?= riscv64-linux-musl-gcc
CFLAGS = -march=rv64imafdc_zicsr_zifencei -mabi=lp64d \
	-nostdlib -nostartfiles -static
USER_LDFLAGS = -Wl,--build-id=none -Wl,-Ttext=0x10000 -Wl,-e,_start
INITRAMFS_DIR = .jitcpu-linux-initramfs

.PHONY: all clean

all: jitcpu-linux-init.cpio jitcpu-linux-smp-init.cpio \
	jitcpu-dining-philosophers.cpio

jitcpu-linux-init: jitcpu-linux-init.S
	$(CC) $(CFLAGS) $(USER_LDFLAGS) -o $@ $<

jitcpu-linux-smp-init: jitcpu-linux-smp-init.S
	$(CC) $(CFLAGS) $(USER_LDFLAGS) -o $@ $<

jitcpu-dining-philosophers: jitcpu-dining-philosophers.c
	$(DINING_CC) -O2 -g0 -static -pthread \
		-march=rv64imafdc_zicsr_zifencei \
		-mabi=lp64d -Wl,--build-id=none -Wall -Wextra -Werror -o $@ $<

%.cpio: %
	rm -rf $(INITRAMFS_DIR)
	mkdir -p $(INITRAMFS_DIR)
	cp $< $(INITRAMFS_DIR)/init
	chmod 755 $(INITRAMFS_DIR)/init
	cd $(INITRAMFS_DIR) && printf './init\n' | \
		cpio --quiet -o -H newc > ../$@
	rm -rf $(INITRAMFS_DIR)

clean:
	rm -rf $(INITRAMFS_DIR)
	rm -f jitcpu-linux-init jitcpu-linux-init.cpio \
		jitcpu-linux-smp-init jitcpu-linux-smp-init.cpio \
		jitcpu-dining-philosophers jitcpu-dining-philosophers.cpio
