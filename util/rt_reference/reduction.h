/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef REDUCTION_H
#define REDUCTION_H

#include <stdint.h>

#define REGION_BYTES (1U << 20)
#define INPUT_BASE 0x82000000UL
#define COPY_BASE 0x83100000UL
#define PATTERN_BASE 0x83000000UL
#define TILE_BYTES (64U << 10)

typedef struct
{
    uint64_t input, output;
    uint32_t bytes, mode;
} ReductionJob;

#endif
