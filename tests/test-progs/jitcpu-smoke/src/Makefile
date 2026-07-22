CROSS_COMPILE ?= riscv64-linux-gnu-
CC = $(CROSS_COMPILE)gcc
CFLAGS = -march=rv64imafdc_zicsr_zifencei -mabi=lp64d \
	-nostdlib -nostartfiles -static
LDFLAGS = -Wl,--build-id=none -Wl,-Ttext=0x80000000 -Wl,-e,_start
USER_LDFLAGS = -Wl,--build-id=none -Wl,-Ttext=0x10000 -Wl,-e,_start
INITRAMFS_DIR = .jitcpu-linux-initramfs

.PHONY: all clean

all: jitcpu-smoke jitcpu-switch jitcpu-repeated-switch \
	jitcpu-linux-init.cpio

jitcpu-smoke: jitcpu-smoke.S
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

jitcpu-switch: jitcpu-switch.S
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

jitcpu-repeated-switch: jitcpu-repeated-switch.S
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

jitcpu-linux-init: jitcpu-linux-init.S
	$(CC) $(CFLAGS) $(USER_LDFLAGS) -o $@ $<

jitcpu-linux-init.cpio: jitcpu-linux-init
	rm -rf $(INITRAMFS_DIR)
	mkdir -p $(INITRAMFS_DIR)
	cp jitcpu-linux-init $(INITRAMFS_DIR)/init
	chmod 755 $(INITRAMFS_DIR)/init
	cd $(INITRAMFS_DIR) && printf './init\n' | \
		cpio --quiet -o -H newc > ../$@
	rm -rf $(INITRAMFS_DIR)

clean:
	rm -rf $(INITRAMFS_DIR)
	rm -f jitcpu-smoke jitcpu-switch jitcpu-repeated-switch \
		jitcpu-linux-init jitcpu-linux-init.cpio
