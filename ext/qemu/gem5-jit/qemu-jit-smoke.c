/*
 * Standalone execution smoke test for the gem5 QEMU JIT adapter
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu-jit.h"

#include <stdio.h>
#include <string.h>

static uint8_t memory[4096];

static int
memory_read(void *opaque, uint64_t address, uint8_t *data, size_t size)
{
    if (address > sizeof(memory) || size > sizeof(memory) - address) {
        return -1;
    }
    memcpy(data, memory + address, size);
    return 0;
}

static int
memory_write(void *opaque, uint64_t address, const uint8_t *data, size_t size)
{
    if (address > sizeof(memory) || size > sizeof(memory) - address) {
        return -1;
    }
    memcpy(memory + address, data, size);
    return 0;
}

static int
memory_map(void *opaque, size_t index, uint64_t *guest_address, uint64_t *size,
           uint8_t **host_address, int *writable)
{
    if (index != 0) {
        return -1;
    }
    *guest_address = 0;
    *size = sizeof(memory);
    *host_address = memory;
    *writable = 1;
    return 0;
}

int
main(void)
{
    /* addi x1,x0,5; addi x2,x1,7; sw x2,256(x0) */
    const uint32_t program[] = { 0x00500093, 0x00708113, 0x10202023 };
    const Gem5QemuJitCallbacks callbacks = {
        .abi_version = GEM5_QEMU_JIT_ABI_VERSION,
        .memory_read = memory_read,
        .memory_write = memory_write,
        .memory_map = memory_map,
    };
    Gem5QemuJitRunResult result;

    memcpy(memory, program, sizeof(program));
    if (gem5_qemu_jit_init(&callbacks) != 0) {
        fprintf(stderr, "adapter initialization failed\n");
        return 1;
    }

    gem5_qemu_jit_set_pc(0);
    if (gem5_qemu_jit_run(3, &result) != 0) {
        fprintf(stderr, "adapter execution failed\n");
        return 1;
    }

    if (result.reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
        result.instructions != 3 || gem5_qemu_jit_get_pc() != 12 ||
        gem5_qemu_jit_get_gpr(1) != 5 ||
        gem5_qemu_jit_get_gpr(2) != 12 || memory[0x100] != 12) {
        fprintf(stderr,
                "unexpected result: reason=%d insns=%llu pc=%#llx "
                "x1=%llu x2=%llu mem[0x100]=%u qemu_exception=%d\n",
                result.reason, (unsigned long long)result.instructions,
                (unsigned long long)gem5_qemu_jit_get_pc(),
                (unsigned long long)gem5_qemu_jit_get_gpr(1),
                (unsigned long long)gem5_qemu_jit_get_gpr(2), memory[0x100],
                result.qemu_exception);
        return 1;
    }

    puts("gem5 QEMU JIT smoke test passed");
    return 0;
}
