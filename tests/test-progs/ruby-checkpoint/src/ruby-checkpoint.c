/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Workload for the Ruby checkpoint regression.
 *
 * Keeps a buffer resident and dirty in the private caches for the whole run,
 * so a checkpoint taken at any point has real writeback work to do, and
 * prints a checksum that only comes out right if every dirty line survived
 * the flush, the serialized trace and the replay on restore.
 */
#include <stdint.h>
#include <stdio.h>

/* 16 KiB, so the buffer stays resident alongside the stack. */
#define WORDS 2048
#define ROUNDS 64

static volatile uint64_t buf[WORDS];

int
main(void)
{
    for (int i = 0; i < WORDS; i++) {
        buf[i] = (uint64_t)i * 2654435761ULL;
    }
    printf("RUBY-CKPT-FILLED\n");
    fflush(stdout);

    for (int r = 0; r < ROUNDS; r++) {
        for (int i = 0; i < WORDS; i++) {
            buf[i] = buf[i] * 6364136223846793005ULL + 1442695040888963407ULL;
        }
    }

    uint64_t sum = 0;
    for (int i = 0; i < WORDS; i++) {
        sum ^= buf[i] + (uint64_t)i;
    }
    printf("RUBY-CKPT-CHECKSUM %016llx\n", (unsigned long long)sum);
    fflush(stdout);
    return 0;
}
