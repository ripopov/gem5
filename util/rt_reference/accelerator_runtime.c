/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "accelerator_runtime.h"

void
start_dma(uint64_t source, uint64_t destination, uint32_t length)
{
    __asm__ volatile("fence iorw, iorw" : : : "memory");
    REG64(48) = source;
    REG64(56) = destination;
    REG32(64) = length;
    __asm__ volatile("fence iorw, iorw" : : : "memory");
    REG32(68) = 1;
    __asm__ volatile("fence iorw, iorw" : : : "memory");
}

int
wait_dma(void)
{
    while (REG32(72) == 1) {}
    __asm__ volatile("fence iorw, iorw" : : : "memory");
    return REG32(72) == 2 ? 0 : 20;
}

int
copy_dma(uint64_t source, uint64_t destination, uint32_t length)
{
    start_dma(source, destination, length);
#ifdef ACCEL_TEST
    uint8_t probe = 0;
    int remote = source >= 0x80000000 && source < 0x84000000;
    if (remote) {
        probe = *(volatile uint8_t *)(uintptr_t)source;
    }
#endif
    while (REG32(72) == 1) {}
    if (REG32(72) != 2) {
        return 20;
    }
    __asm__ volatile("fence iorw, iorw" : : : "memory");
#ifdef ACCEL_TEST
    if (remote && probe != *(volatile uint8_t *)(uintptr_t)destination) {
        return 25;
    }
#endif
    return 0;
}

_Noreturn void
accelerator_loop(int (*kernel)(uint64_t))
{
    __asm__ volatile("csrw mie, %0" : : "r"(1UL << 11) : "memory");
    for (;;) {
        if (REG32(20) != 1) {
            __asm__ volatile("wfi" : : : "memory");
            continue;
        }
        uint64_t job = REG64(32);
        REG32(24) = 1;
        int error = kernel(job);
        __asm__ volatile("fence iorw, iorw" : : : "memory");
        REG32(40) = error;
        __asm__ volatile("fence iorw, iorw" : : : "memory");
        while (REG32(44)) {}
    }
}
