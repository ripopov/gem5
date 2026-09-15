/* SPDX-License-Identifier: BSD-3-Clause */

OUTPUT_ARCH("riscv")
ENTRY(_start)
SECTIONS {
 . = 0x80000000;
 .text : { *(.text.init) *(.text .text.*) }
 .rodata : { *(.rodata .rodata.*) }
 .data : { *(.data .data.*) *(.sdata .sdata.*) }
 .rom 0x90000000 : { QUAD(17) }
 .bss 0x80010000 : { *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) }
}
