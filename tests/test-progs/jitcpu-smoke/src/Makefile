CROSS_COMPILE ?= riscv64-linux-gnu-
CC = $(CROSS_COMPILE)gcc
CFLAGS = -march=rv64imafdc_zicsr_zifencei -mabi=lp64d \
	-nostdlib -nostartfiles -static
LDFLAGS = -Wl,--build-id=none -Wl,-Ttext=0x80000000 -Wl,-e,_start

.PHONY: all clean

all: jitcpu-smoke jitcpu-switch

jitcpu-smoke: jitcpu-smoke.S
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

jitcpu-switch: jitcpu-switch.S
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f jitcpu-smoke jitcpu-switch
