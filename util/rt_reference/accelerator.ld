/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
OUTPUT_ARCH(riscv)
ENTRY(_start)
MEMORY {
    CODE (rx) : ORIGIN = 0x40000000, LENGTH = 64K
    DATA (rw) : ORIGIN = 0x40010000, LENGTH = 192K
}
SECTIONS
{
    .text : { KEEP(*(.text.init)) *(.text .text.*) } > CODE
    .rodata : {
        *(.rodata .rodata.* .srodata .srodata.* .eh_frame .eh_frame.*)
        __code_end = .;
    } > CODE
    .data : {
        __data_start = .;
        *(.data .data.*)
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.*)
    } > DATA
    .bss (NOLOAD) : {
        . = ALIGN(8);
        __bss_start = .;
        *(.sbss .sbss.* .bss .bss.*) *(COMMON)
        . = ALIGN(8);
        __bss_end = .;
    } > DATA
    .stack (NOLOAD) : {
        . = ALIGN(16);
        . += 16384;
        __stack_top = .;
    } > DATA
    ASSERT(__code_end <= ORIGIN(CODE) + LENGTH(CODE),
           "Accelerator code and constants exceed 64 KiB Code TCM")
    ASSERT(__stack_top <= ORIGIN(DATA) + LENGTH(DATA),
           "Accelerator writable data and stack exceed 192 KiB Data TCM")
}
