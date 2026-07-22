OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    . = 0x02000000;
    .text : ALIGN(16) { KEEP(*(.text.init)) }
    . = 0x02001000;
    .text.resume : ALIGN(16) { KEEP(*(.text.resume)) }
    /DISCARD/ : { *(.comment .note .eh_frame) }
}
