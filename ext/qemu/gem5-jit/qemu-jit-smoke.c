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
    /* addi x1,x0,5; addi x2,x1,7; csrr x3,mhartid; sw x2,256(x0) */
    const uint32_t program[] = {
        0x00500093, 0x00708113, 0xf14021f3, 0x10202023
    };
    const Gem5QemuJitCallbacks callbacks[] = {
        {
            .abi_version = GEM5_QEMU_JIT_ABI_VERSION,
            .instance_id = 0,
            .instance_count = 2,
            .hart_id = 7,
            .memory_read = memory_read,
            .memory_write = memory_write,
            .memory_map = memory_map,
        },
        {
            .abi_version = GEM5_QEMU_JIT_ABI_VERSION,
            .instance_id = 1,
            .instance_count = 2,
            .hart_id = 11,
            .memory_read = memory_read,
            .memory_write = memory_write,
            .memory_map = memory_map,
        },
    };
    Gem5QemuJitRunResult results[2];
    unsigned instance;

    memcpy(memory, program, sizeof(program));
    for (instance = 0; instance < 2; ++instance) {
        uint64_t value;

        if (gem5_qemu_jit_init(&callbacks[instance]) != 0) {
            fprintf(stderr, "adapter instance %u initialization failed\n",
                    instance);
            return 1;
        }
        /*
         * SSTC needs a QEMU timer to post STIP asynchronously, but gem5 owns
         * the event queue and CLINT interrupts. The embedded CPU must not
         * advertise an unsynchronized internal timer implementation.
         */
        if (gem5_qemu_jit_get_csr(instance, 0x14d, &value) == 0) {
            fprintf(stderr, "adapter instance %u unexpectedly exposes sstc\n",
                    instance);
            return 1;
        }
        gem5_qemu_jit_set_pc(instance, 0);
        if (gem5_qemu_jit_set_gpr(instance, 4, 0x44 + instance) != 0) {
            fprintf(stderr, "adapter instance %u register setup failed\n",
                    instance);
            return 1;
        }
    }

    for (instance = 0; instance < 2; ++instance) {
        unsigned other = instance ^ 1;

        if (gem5_qemu_jit_run(instance, 4, &results[instance]) != 0) {
            fprintf(stderr, "adapter instance %u execution failed\n",
                    instance);
            return 1;
        }

        if (gem5_qemu_jit_get_pc(other) != (instance == 0 ? 0 : 16) ||
            gem5_qemu_jit_get_gpr(other, 4) != 0x44 + other) {
            fprintf(stderr,
                    "instance %u execution corrupted instance %u: "
                    "pc=%#llx x4=%#llx\n",
                    instance, other,
                    (unsigned long long)gem5_qemu_jit_get_pc(other),
                    (unsigned long long)gem5_qemu_jit_get_gpr(other, 4));
            return 1;
        }
    }

    for (instance = 0; instance < 2; ++instance) {
        if (results[instance].reason != GEM5_QEMU_JIT_EXIT_BUDGET ||
            results[instance].instructions != 4 ||
            gem5_qemu_jit_get_pc(instance) != 16 ||
            gem5_qemu_jit_get_gpr(instance, 1) != 5 ||
            gem5_qemu_jit_get_gpr(instance, 2) != 12 ||
            gem5_qemu_jit_get_gpr(instance, 3) !=
                callbacks[instance].hart_id ||
            gem5_qemu_jit_get_gpr(instance, 4) != 0x44 + instance) {
            fprintf(stderr,
                    "unexpected instance %u result: reason=%d insns=%llu "
                    "pc=%#llx x1=%llu x2=%llu x3=%llu x4=%#llx "
                    "qemu_exception=%d\n",
                    instance, results[instance].reason,
                    (unsigned long long)results[instance].instructions,
                    (unsigned long long)gem5_qemu_jit_get_pc(instance),
                    (unsigned long long)gem5_qemu_jit_get_gpr(instance, 1),
                    (unsigned long long)gem5_qemu_jit_get_gpr(instance, 2),
                    (unsigned long long)gem5_qemu_jit_get_gpr(instance, 3),
                    (unsigned long long)gem5_qemu_jit_get_gpr(instance, 4),
                    results[instance].qemu_exception);
            return 1;
        }
    }
    if (memory[0x100] != 12) {
        fprintf(stderr, "unexpected shared memory value: %u\n", memory[0x100]);
        return 1;
    }

    puts("gem5 QEMU JIT two-hart smoke test passed");
    return 0;
}
