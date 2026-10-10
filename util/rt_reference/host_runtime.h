/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef HOST_RUNTIME_H
#define HOST_RUNTIME_H

#include <stdint.h>

void host_submit_and_wait(uint8_t jobs[][64], uint32_t cores,
                          uint32_t expected_status, const uint8_t *probe);

#endif
