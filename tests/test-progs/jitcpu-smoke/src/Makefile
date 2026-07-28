# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause

CROSS_COMPILE ?= riscv64-linux-gnu-
CC = $(CROSS_COMPILE)gcc
DINING_CC ?= riscv64-linux-musl-gcc
DINING_SWITCHES ?= 5
DINING_OTHER_PHASE_MEALS ?= 256
# The wider SMP variants share the four-hart sources. A smaller per-hart
# footprint keeps the serialized trace-driven FLUSH worklist proportional to
# the four-hart runs while covering two to four times as many requesters.
WIDE_SMP_LINES_PER_HART ?= 256
CFLAGS = -march=rv64imafdc_zicsr_zifencei -mabi=lp64d \
	-nostdlib -nostartfiles -static
LDFLAGS = -Wl,--build-id=none -Wl,-Ttext=0x80000000 -Wl,-e,_start
USER_LDFLAGS = -Wl,--build-id=none -Wl,-Ttext=0x10000 -Wl,-e,_start
INITRAMFS_DIR = .jitcpu-linux-initramfs

.PHONY: all clean jitcpu-dining-philosophers-repeated

all: jitcpu-smoke jitcpu-switch jitcpu-repeated-switch \
	jitcpu-smp-repeated-switch jitcpu-smp-wfi-repeated-switch \
	jitcpu-smp8-repeated-switch \
	jitcpu-mesh16-repeated-switch jitcpu-mesh16-wfi-repeated-switch \
	jitcpu-16core-mesh-switch jitcpu-linux-init.cpio \
	jitcpu-linux-smp-init.cpio jitcpu-dining-philosophers.cpio \
	jitcpu-dining-philosophers-repeated.cpio

jitcpu-smoke: jitcpu-smoke.S
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

jitcpu-switch: jitcpu-switch.S
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

jitcpu-repeated-switch: jitcpu-repeated-switch.S
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

jitcpu-smp-repeated-switch: jitcpu-smp-repeated-switch.S
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

jitcpu-smp-wfi-repeated-switch: jitcpu-smp-wfi-repeated-switch.S
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

jitcpu-smp8-repeated-switch: jitcpu-smp-repeated-switch.S
	$(CC) $(CFLAGS) $(LDFLAGS) -DJITCPU_NUM_HARTS=8 \
		-DJITCPU_LINES_PER_HART=$(WIDE_SMP_LINES_PER_HART) -o $@ $<

jitcpu-mesh16-repeated-switch: jitcpu-smp-repeated-switch.S
	$(CC) $(CFLAGS) $(LDFLAGS) -DJITCPU_NUM_HARTS=16 \
		-DJITCPU_LINES_PER_HART=$(WIDE_SMP_LINES_PER_HART) -o $@ $<

jitcpu-mesh16-wfi-repeated-switch: jitcpu-smp-wfi-repeated-switch.S
	$(CC) $(CFLAGS) $(LDFLAGS) -DJITCPU_NUM_HARTS=16 \
		-DJITCPU_LINES_PER_HART=$(WIDE_SMP_LINES_PER_HART) -o $@ $<

jitcpu-16core-mesh-switch: jitcpu-16core-mesh-switch.S
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

jitcpu-linux-init: jitcpu-linux-init.S
	$(CC) $(CFLAGS) $(USER_LDFLAGS) -o $@ $<

jitcpu-linux-smp-init: jitcpu-linux-smp-init.S
	$(CC) $(CFLAGS) $(USER_LDFLAGS) -o $@ $<

jitcpu-dining-philosophers: jitcpu-dining-philosophers.c
	$(DINING_CC) -O2 -g0 -static -pthread \
		-march=rv64imafdc_zicsr_zifencei \
		-mabi=lp64d -Wl,--build-id=none -Wall -Wextra -Werror -o $@ $<

jitcpu-dining-philosophers-repeated: jitcpu-dining-philosophers.c
	$(DINING_CC) -O2 -g0 -static -pthread \
		-march=rv64imafdc_zicsr_zifencei \
		-mabi=lp64d -Wl,--build-id=none -Wall -Wextra -Werror \
		-DJITCPU_DINING_SWITCHES=$(DINING_SWITCHES) \
		-DJITCPU_DINING_OTHER_PHASE_MEALS=$(DINING_OTHER_PHASE_MEALS) \
		-o $@ $<

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
	rm -f jitcpu-smoke jitcpu-switch jitcpu-repeated-switch \
		jitcpu-smp-repeated-switch jitcpu-smp-wfi-repeated-switch \
		jitcpu-smp8-repeated-switch \
		jitcpu-mesh16-repeated-switch jitcpu-mesh16-wfi-repeated-switch \
		jitcpu-16core-mesh-switch \
		jitcpu-linux-init jitcpu-linux-init.cpio \
		jitcpu-linux-smp-init jitcpu-linux-smp-init.cpio \
		jitcpu-dining-philosophers jitcpu-dining-philosophers.cpio \
		jitcpu-dining-philosophers-repeated \
		jitcpu-dining-philosophers-repeated.cpio
