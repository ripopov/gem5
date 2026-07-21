OUTPUT_ARCH(riscv)
ENTRY(_start)

/* Each SCR1 instance has a private TCM, so both harts may use this stack. */
__stack_top = 0x00490000;

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x00000200;
    .boot :
    {
        KEEP(*(.boot))
    } :text

    .text :
    {
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } :text

    .data :
    {
        *(.data .data.*)
        *(.sdata .sdata.*)
    } :data

    .bss (NOLOAD) :
    {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
    } :data

    ASSERT(. < 0x00010000, "gem5 validation image is unexpectedly large")

    /DISCARD/ :
    {
        *(.riscv.attributes)
        *(.comment)
    }
}
