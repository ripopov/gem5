/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef ACCELERATOR_RUNTIME_H
#define ACCELERATOR_RUNTIME_H

#include <stdint.h>

#define REG32(offset) (*(volatile uint32_t *)(0x10000000UL + (offset)))
#define REG64(offset) (*(volatile uint64_t *)(0x10000000UL + (offset)))

void start_dma(uint64_t source, uint64_t destination, uint32_t length);
int wait_dma(void);
int copy_dma(uint64_t source, uint64_t destination, uint32_t length);
_Noreturn void accelerator_loop(int (*kernel)(uint64_t));

#endif
