# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
CROSS ?= riscv64-unknown-elf-
PICOLIBC ?= /usr/lib/picolibc/riscv64-unknown-elf
CC = $(CROSS)gcc
FLAGS = -march=rv64imafdc -mabi=lp64d -mcmodel=medany -O3 -g \
        -ffreestanding -fno-builtin -ffp-contract=off \
        -Wall -Wextra -Werror -I../../include -I$(PICOLIBC)/include

all: out/host.elf out/host-gem5.elf out/accelerator.elf

out/host.elf: host.c renderer.c renderer.h start.S link.ld Makefile \
              ../m5/src/abi/riscv/m5op.S \
              ../../include/gem5/asm/generic/m5ops.h
	@mkdir -p out
	$(CC) $(FLAGS) -nostdlib -Wl,-T,link.ld,-Map,out/host.map \
	    start.S host.c renderer.c ../../util/m5/src/abi/riscv/m5op.S \
	    -L$(PICOLIBC)/lib -lm -lc -lgcc -o $@
	$(CROSS)size $@

out/host-gem5.elf: host.c host_runtime.c host_runtime.h renderer.c renderer.h \
                   start.S link.ld Makefile
	@mkdir -p out
	$(CC) $(FLAGS) -DACCEL_HOST -nostdlib \
	    -Wl,-T,link.ld,-Map,out/host-gem5.map \
	    start.S host.c host_runtime.c renderer.c ../m5/src/abi/riscv/m5op.S \
	    -L$(PICOLIBC)/lib -lm -lc -lgcc -o $@
	$(CROSS)size $@

out/host-render.elf: host.c renderer.c renderer.h start.S link.ld Makefile \
                     ../m5/src/abi/riscv/m5op.S \
                     ../../include/gem5/asm/generic/m5ops.h
	@mkdir -p out
	$(CC) $(FLAGS) -DHOST_RENDER_STATS -nostdlib \
	    -Wl,-T,link.ld,-Map,out/host-render.map \
	    start.S host.c renderer.c ../m5/src/abi/riscv/m5op.S \
	    -L$(PICOLIBC)/lib -lm -lc -lgcc -o $@
	$(CROSS)size $@

out/accelerator.elf: accelerator.c accelerator_runtime.c \
                     accelerator_runtime.h renderer.c renderer.h \
                     start.S accelerator.ld Makefile
	@mkdir -p out
	$(CC) $(FLAGS) -nostdlib \
	    -Wl,-T,accelerator.ld,-Map,out/accelerator.map \
	    start.S accelerator.c accelerator_runtime.c renderer.c \
	    -L$(PICOLIBC)/lib -lm -lc -lgcc -o $@
	$(CROSS)size $@

out/host-test.elf: host.c host_runtime.c host_runtime.h renderer.c renderer.h \
                   start.S link.ld Makefile
	@mkdir -p out
	$(CC) $(FLAGS) -DACCEL_HOST -DACCEL_TEST -nostdlib \
	    -Wl,-T,link.ld,-Map,out/host-test.map \
	    start.S host.c host_runtime.c renderer.c ../m5/src/abi/riscv/m5op.S \
	    -L$(PICOLIBC)/lib -lm -lc -lgcc -o $@

out/accelerator-test.elf: accelerator.c accelerator_runtime.c \
                          accelerator_runtime.h renderer.c renderer.h \
                          start.S accelerator.ld Makefile
	@mkdir -p out
	$(CC) $(FLAGS) -DACCEL_TEST -nostdlib \
	    -Wl,-T,accelerator.ld,-Map,out/accelerator-test.map \
	    start.S accelerator.c accelerator_runtime.c renderer.c \
	    -L$(PICOLIBC)/lib -lm -lc -lgcc -o $@

backend-tests: out/host-test.elf out/accelerator-test.elf

out/smrnmi-present.elf out/smrnmi-absent.elf: smrnmi.S link.ld Makefile \
                                         ../m5/src/abi/riscv/m5op.S
	@mkdir -p out
	$(CC) $(FLAGS) -DEXPECT_SMRNMI=$(if $(findstring present,$@),1,0) \
	    -nostdlib -Wl,-T,link.ld smrnmi.S \
	    ../m5/src/abi/riscv/m5op.S -o $@

extension-tests: out/smrnmi-present.elf out/smrnmi-absent.elf

out/native: host.c renderer.c renderer.h Makefile
	@mkdir -p out
	cc -std=c11 -O3 -g -ffp-contract=off -Wall -Wextra -Werror \
	    -DNATIVE host.c renderer.c -lm -o $@

out/test: test.c renderer.c renderer.h Makefile
	@mkdir -p out
	cc -std=c11 -O2 -g -ffp-contract=off -Wall -Wextra -Werror \
	    test.c renderer.c -lm -o $@

check: out/test
	./out/test

.PHONY: all check backend-tests extension-tests

# Separate kernels share the same resident job/DMA and host completion runtime.
out/reduction-host.elf: reduction_host.c reduction.h host_runtime.c \
                        host_runtime.h start.S link.ld Makefile
	@mkdir -p out
	$(CC) $(FLAGS) -nostdlib -Wl,-T,link.ld,-Map,out/reduction-host.map \
	    start.S reduction_host.c host_runtime.c ../m5/src/abi/riscv/m5op.S \
	    -L$(PICOLIBC)/lib -lc -lgcc -o $@
	$(CROSS)size $@

out/reduction-accelerator.elf: reduction_accelerator.c reduction.h \
                               accelerator_runtime.c accelerator_runtime.h \
                               start.S accelerator.ld Makefile
	@mkdir -p out
	$(CC) $(FLAGS) -nostdlib \
	    -Wl,-T,accelerator.ld,-Map,out/reduction-accelerator.map \
	    start.S reduction_accelerator.c accelerator_runtime.c \
	    -L$(PICOLIBC)/lib -lc -lgcc -o $@
	$(CROSS)size $@

reduction: out/reduction-host.elf out/reduction-accelerator.elf

.PHONY: reduction
