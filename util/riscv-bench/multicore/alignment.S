# SPDX-License-Identifier: BSD-3-Clause

.section .text.init,"ax"
.globl _start
.option norvc
_start:
    csrr t0, mhartid
    bnez t0, park
    la t0, trap
    csrw mtvec, t0
    la s0, data
    ld t0, 0(s0)  # warm the normal-load translation
    li t0, 17
    sd t0, 0(s0)
#if CASE == 4
    addi s0, s0, 61
#else
    addi s0, s0, 1
#endif
#if CASE == 1
    li s1, 4
    lr.d t0, (s0)
#elif CASE == 2 || CASE == 4
    li s1, 6
    amoadd.d t0, zero, (s0)
#elif CASE == 3
    addi s0, s0, -1
    lr.d t0, (s0)
    addi s0, s0, 1
    li s1, 6
    sc.d t0, t0, (s0)
#endif
    j fail
trap:
    csrr t0, mcause
    bne t0, s1, fail
    csrr t0, mtval
    bne t0, s0, fail
    li a0, 0
    .word 0x4200007b
park: j park
fail:
    li a0, 0
    li a1, 70
    .word 0x4400007b
    j fail
.section .data
.balign 64
data: .space 128
