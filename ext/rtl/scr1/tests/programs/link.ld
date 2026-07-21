OUTPUT_ARCH(riscv)
ENTRY(_start)

__stack_top = 0x00490000;
__stack_limit = __stack_top - 0x400;

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
        *(.srodata .srodata.*)
    } :tcm

    .data :
    {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } :tcm

    .bss (NOLOAD) :
    {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
    } :tcm

    ASSERT(. <= __stack_limit, "TCM image overlaps reserved stack")

    /DISCARD/ :
    {
        *(.riscv.attributes)
        *(.comment)
    }
}
