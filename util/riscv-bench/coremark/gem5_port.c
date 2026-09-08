/* Copyright (c) 2026 Roman Popov
   SPDX-License-Identifier: BSD-3-Clause

   Platform glue for running CoreMark bare-metal on gem5's HiFive platform.

   The benchmark needs a character sink and a way to stop the simulation.
   Characters go to the 8250 UART that HiFive maps at 0x10000000, which gem5
   forwards to the simulated terminal; the exit is gem5's m5_exit pseudo
   instruction (opcode 0x7b, function 0x21 in bits 31:25). */

#include <stdint.h>

#define UART_BASE 0x10000000UL

void
gem5_putchar(char c)
{
    *(volatile uint8_t *)UART_BASE = (uint8_t)c;
}

void
gem5_exit(int code)
{
    register unsigned long a0 asm("a0") = (unsigned long)code;
    asm volatile(".word 0x4200007b" : : "r"(a0) : "memory");
    for (;;)
        ;
}
