/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stddef.h>

#include <gem5/m5ops.h>

#include "host_runtime.h"
#include "reduction.h"

#define DMA_DATA __attribute__((section(".dma"), aligned(64)))
static DMA_DATA uint8_t jobs[32][64];
static DMA_DATA float partials[32];
static float result;

static _Noreturn void
fail(unsigned code)
{
    m5_fail(0, code);
    for (;;) {}
}

int
main(void)
{
    uint32_t mode;
    if (m5_read_file(&mode, sizeof(mode), 0) != sizeof(mode)) {
        fail(92);
    }
    unsigned cores = *(volatile uint32_t *)0x20000108UL;
    if (!cores || cores > 16 || (mode != 0 && cores != 1)) {
        fail(94);
    }
    __asm__ volatile("csrw mie, %0" : : "r"(1UL << 11) : "memory");
    for (unsigned i = 0; i < cores; ++i) {
        ReductionJob *job = (ReductionJob *)jobs[i];
        job->input = INPUT_BASE + i * REGION_BYTES;
        job->output = (uintptr_t)&partials[i];
        job->bytes = REGION_BYTES;
        job->mode = mode;
        if (mode == 1) {
            job->output = COPY_BASE;
        } else if (mode == 3) {
            job->output = COPY_BASE;
            job->bytes = 4096;
        } else if (mode == 2) {
            job->input = PATTERN_BASE + 17;
            job->output = COPY_BASE + 5;
            job->bytes = 8193;
        }
    }
    host_submit_and_wait(jobs, cores, 2, NULL);
    if (mode == 0) {
        for (unsigned i = 0; i < cores; ++i) {
            float value = *(volatile float *)&partials[i];
            if (value != 262144.0f) {
                fail(130 + i);
            }
            result += value;
        }
        if (result != (float)(cores * (REGION_BYTES / sizeof(float)))) {
            fail(150);
        }
        m5_write_file(partials, cores * sizeof(float), 0, "partials.bin");
        m5_write_file(&result, sizeof(result), 0, "sum.bin");
    } else {
        ReductionJob *job = (ReductionJob *)jobs[0];
        volatile uint8_t *output = (volatile uint8_t *)job->output;
        /* The harness verifies every exported byte. The throughput probe
         * checks the edge cache lines here so uncached host validation does
         * not add a million serialized memory accesses to the experiment. */
        unsigned output_bytes = mode == 3 ? 4 : job->bytes;
        unsigned checked = mode == 1 ? 128 : output_bytes;
        for (unsigned check = 0; check < checked; ++check) {
            unsigned i =
                mode == 1 && check >= 64 ? job->bytes - 128 + check : check;
            uint8_t expected;
            if (mode != 2) {
                const uint8_t one[] = {0, 0, 128, 63};
                expected = one[i % 4];
            } else {
                unsigned index = i + 17;
                expected = (37 * index + index / 64) & 255;
            }
            if (output[i] != expected) {
                fail(151);
            }
        }
        if (mode == 2 && (output[-1] || output[job->bytes])) {
            fail(152);
        }
        m5_write_file((void *)output, output_bytes, 0, "copy.bin");
    }
    m5_write_file(jobs, cores * 64, 0, "jobs.bin");
    m5_exit(0);
    for (;;) {}
}
