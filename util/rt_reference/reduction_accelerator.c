/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "accelerator_runtime.h"
#include "reduction.h"

static _Alignas(64) float tiles[2][TILE_BYTES / sizeof(float)];
static _Alignas(64) ReductionJob job;
static _Alignas(64) float partial;

static float
sum_tile(const float *values, unsigned count)
{
    /* Independent scalar lanes remove the long FP recurrence without RVV
     * or reassociation by the compiler. Every operation is binary32. */
    float a = 0, b = 0, c = 0, d = 0;
    float e = 0, f = 0, g = 0, h = 0;
    for (unsigned i = 0; i < count; i += 8) {
        a += values[i];
        b += values[i + 1];
        c += values[i + 2];
        d += values[i + 3];
        e += values[i + 4];
        f += values[i + 5];
        g += values[i + 6];
        h += values[i + 7];
    }
    return ((a + b) + (c + d)) + ((e + f) + (g + h));
}

static int
reduce(void)
{
    unsigned count = job.bytes / TILE_BYTES;
    partial = 0.0f;
    start_dma(job.input, (uintptr_t)tiles[0], TILE_BYTES);
    for (unsigned tile = 0; tile < count; ++tile) {
        int error = wait_dma();
        if (error) {
            return error;
        }
        if (tile + 1 < count) {
            start_dma(job.input + (tile + 1) * TILE_BYTES,
                      (uintptr_t)tiles[(tile + 1) % 2], TILE_BYTES);
        }
        partial += sum_tile(tiles[tile % 2], TILE_BYTES / sizeof(float));
    }
    return copy_dma((uintptr_t)&partial, job.output, sizeof(partial));
}

static int
copy(void)
{
    for (unsigned offset = 0; offset < job.bytes; offset += TILE_BYTES) {
        unsigned size = job.bytes - offset;
        if (size > TILE_BYTES) {
            size = TILE_BYTES;
        }
        int error = copy_dma(job.input + offset, (uintptr_t)tiles, size);
        if (!error) {
            error = copy_dma((uintptr_t)tiles, job.output + offset, size);
        }
        if (error) {
            return error;
        }
    }
    return 0;
}

static int
check_dma(void)
{
    volatile unsigned char *data = (volatile unsigned char *)tiles;
    /* Warm and dirty a destination line before an unaligned DMA overwrite. */
    for (unsigned i = 0; i < 64; ++i) {
        data[i] = 0xa5;
    }
    int error = copy_dma(job.input, (uintptr_t)tiles + 3, job.bytes);
    if (error) {
        return error;
    }
    if (data[0] != 0xa5 || data[1] != 0xa5 || data[2] != 0xa5) {
        return 35;
    }
    for (unsigned i = 0; i < job.bytes; ++i) {
        unsigned source = 17 + i;
        if (data[3 + i] != ((37 * source + source / 64) & 255)) {
            return 36;
        }
    }
    /* DMA reads must observe dirty CPU bytes, including sub-line reads. */
    unsigned char original = data[3];
    data[3] = original ^ 255;
    error =
        copy_dma((uintptr_t)tiles + 3, (uintptr_t)tiles + TILE_BYTES + 7, 4);
    if (error || data[TILE_BYTES + 7] != (original ^ 255)) {
        return 37;
    }
    /* Preserve dirty neighbors when DMA overwrites only one byte. */
    data[0] = 0x5a;
    unsigned char neighbor = data[4];
    error = copy_dma(job.input, (uintptr_t)tiles + 4, 1);
    if (error || data[0] != 0x5a || data[3] != (original ^ 255) ||
        data[4] != original) {
        return 38;
    }
    data[3] = original;
    data[4] = neighbor;
    error = copy_dma((uintptr_t)tiles + 3, job.output, job.bytes);
    if (error) {
        return error;
    }
    const uint64_t invalid[][3] = {
        {0x40000000, (uintptr_t)tiles, 64}, /* Code TCM */
        {UINT64_MAX - 16, (uintptr_t)tiles, 64},
        {(uintptr_t)tiles, (uintptr_t)tiles + 4, 64}, /* overlap */
        {(uintptr_t)tiles, job.output, 0},
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        start_dma(invalid[i][0], invalid[i][1], invalid[i][2]);
        if (REG32(72) != 3) {
            return 31;
        }
    }
    /* A terminal error must allow a new command. */
    return copy_dma((uintptr_t)tiles, (uintptr_t)tiles + 16, 4);
}

static int
check_faults(void)
{
    /* Each command has many accepted transactions when the first error
     * returns. Status must remain busy until the accepted requests drain. */
    start_dma(0x83f00000, (uintptr_t)tiles, 4096);
    if (wait_dma() != 20) {
        return 32;
    }
    if (copy_dma(INPUT_BASE, (uintptr_t)tiles, 4)) {
        return 33;
    }
    start_dma((uintptr_t)tiles, 0x83f00000, 4096);
    if (wait_dma() != 20) {
        return 34;
    }
    return copy_dma((uintptr_t)tiles, job.output, 4);
}

static int
execute(uint64_t address)
{
    if (address % 8 || address < 0x80000000 ||
        address > 0x84000000 - sizeof(job)) {
        return 21;
    }
    int error = copy_dma(address, (uintptr_t)&job, sizeof(job));
    if (error) {
        return error;
    }
    if (job.input < INPUT_BASE || job.input > 0x84000000 - job.bytes ||
        job.output < 0x81000000 || job.output > 0x84000000 - job.bytes) {
        return 22;
    }
    if (job.mode == 0 && job.bytes == REGION_BYTES) {
        return reduce();
    }
    if (job.mode == 1 && job.bytes == REGION_BYTES) {
        return copy();
    }
    if (job.mode == 2 && job.bytes == 8193) {
        return check_dma();
    }
    if (job.mode == 3 && job.bytes == 4096) {
        return check_faults();
    }
    return 23;
}

int
main(void)
{
    accelerator_loop(execute);
}
