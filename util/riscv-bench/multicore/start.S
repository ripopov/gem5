# SPDX-License-Identifier: BSD-3-Clause

.section .text.init,"ax"
.global _start
_start:
    csrr a0, mhartid
    la sp, stacks
    addi t0, a0, 1
    slli t0, t0, 12
    add sp, sp, t0
    la t0, trap
    csrw mtvec, t0
    li t0, 0x6600
    csrs mstatus, t0
    call test_main
1:  j 1b
.balign 4
trap:
    csrr a1, mcause
    addi a1, a1, 128
    li a0, 0
    .word 0x4400007b
    j trap
.section .bss
.balign 16
stacks: .space 4096 * NHARTS
