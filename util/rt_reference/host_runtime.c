/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <gem5/m5ops.h>

#include "host_runtime.h"

void
host_submit_and_wait(uint8_t jobs[][64], uint32_t accelerator_cores,
                     uint32_t expected_status, const uint8_t *probe)
{
    /* Stat events run one simulation quantum after the pseudo op. Wait
     * for the global reset before any doorbell can wake an accelerator.
     * init_param supplies the quantum rounded up to nanoseconds. */
    uint64_t stats_sync_ns = m5_init_param(0, 0) + 1;
    __asm__ volatile("fence iorw, iorw" : : : "memory");
    m5_reset_stats(0, 0);
    m5_quiesce_ns(stats_sync_ns);
    for (unsigned i = 0; i < accelerator_cores; ++i) {
        uintptr_t base = 0x10000000UL + 0x1000 * i;
        *(volatile uint64_t *)(base + 8) = 0x20000000UL + 4 * i;
        *(volatile uint32_t *)(base + 16) = i + 1;
        *(volatile uint64_t *)base = (uintptr_t)jobs[i];
    }
    /* Immutable descriptor reads contend with accelerator DMA on queue 0. */
    if (probe) {
        for (unsigned i = 0; i < 1024; ++i) {
            (void)*(volatile uint8_t *)(probe + (i % 56));
        }
    }
    volatile uint32_t *pending = (volatile uint32_t *)0x20000100UL;
    uint32_t completed = 0;
    uint32_t expected = UINT32_MAX >> (32 - accelerator_cores);
    while (completed != expected) {
        uint32_t observed = *pending;
        if (observed) {
            completed |= observed;
            *(volatile uint32_t *)0x20000104UL = observed;
        } else {
            __asm__ volatile("wfi" : : : "memory");
        }
    }
    for (unsigned i = 0; i < accelerator_cores; ++i) {
        if (*(volatile uint32_t *)(0x10000014UL + 0x1000 * i) !=
            expected_status) {
            m5_fail(0, 100 + i);
            for (;;) {}
        }
    }
    __asm__ volatile("fence iorw, iorw" : : : "memory");
    /* Save the completed batch before host image checking/export. Reset
     * again so the automatic exit dump contains only subsequent work. */
    m5_dump_reset_stats(0, 0);
    m5_quiesce_ns(stats_sync_ns);
}
