/*
 * Copyright (c) 2026 Roman Popov
 * SPDX-License-Identifier: BSD-3-Clause
 */
    .section .text.init
    .globl _start
_start:
    li s0, 0
    la sp, __stack_top
    la t0, trap
    csrw mtvec, t0
    csrr t0, misa
    li t1, 0x1005 /* M, A and C must be reported. */
    and t0, t0, t1
    bne t0, t1, fail

#if EXPECT_SMRNMI
    /* Smrnmi resets NMIE to zero and permits access to mnstatus. */
    csrr t0, 0x744
    andi t0, t0, 8
    bnez t0, fail
    li t0, 8
    csrs 0x744, t0
    csrr t0, 0x744
    andi t0, t0, 8
    beqz t0, fail
    j pass
#else
    /* With Smrnmi absent, mnstatus must raise an illegal instruction. */
    li s0, 1
    csrr t0, 0x744
    j fail
#endif

    .balign 4
trap:
    beqz s0, fail
    csrr t0, mcause
    li t1, 2
    bne t0, t1, fail
pass:
    li a0, 0
    call m5_exit
    j pass
fail:
    li a0, 0
    li a1, 1
    call m5_fail
    j fail
