OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS
{
    . = 0x02000000;
    .text : ALIGN(16)
    {
        KEEP(*(.text.init))
        *(.text .text.*)
    }
    .rodata : ALIGN(16) { *(.rodata .rodata.*) }
    PROVIDE(__global_pointer$ = . + 0x800);
    .data : ALIGN(16) { *(.data .data.* .sdata .sdata.*) }
    .bss : ALIGN(16)
    {
        __bss_start = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)
        __bss_end = .;
    }
    /DISCARD/ : { *(.comment .note .eh_frame) }
}
