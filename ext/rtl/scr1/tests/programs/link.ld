OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
    external PT_LOAD FLAGS(5);
    tcm PT_LOAD FLAGS(5);
}

SECTIONS
{
    . = 0x00000200;
    .boot :
    {
        KEEP(*(.boot))
    } :external

    . = 0x00480000;
    .text :
    {
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.*)
        *(.bss .bss.*)
    } :tcm

    /DISCARD/ :
    {
        *(.riscv.attributes)
        *(.comment)
    }
}
