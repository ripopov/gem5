/* Minimal startup for the freestanding C integration test. */

    .section .boot, "ax"
    .global _start
    .type _start, @function
_start:
    lui     sp, %hi(__stack_top)
    addi    sp, sp, %lo(__stack_top)
    call    c_integration_test

1:
    j       1b
    .size _start, . - _start
