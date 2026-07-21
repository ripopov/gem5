/* Shared-memory startup for two SCR1 harts with private TCM stacks. */

    .section .boot, "ax"
    .global _start
    .type _start, @function
_start:
    lui     sp, %hi(__stack_top)
    addi    sp, sp, %lo(__stack_top)
    call    gem5_test_main

1:
    j       1b
    .size _start, . - _start
