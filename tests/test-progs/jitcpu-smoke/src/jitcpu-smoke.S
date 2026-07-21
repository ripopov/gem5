/* SPDX-License-Identifier: BSD-3-Clause */

    .section .text.init
    .option norvc
    .globl _start
_start:
    li      t0, 0x10000000
    li      t1, 'J'
    sb      t1, 0(t0)
    li      t1, 'I'
    sb      t1, 0(t0)
    li      t1, 'T'
    sb      t1, 0(t0)
    li      t1, '\n'
    sb      t1, 0(t0)

    li      s0, 0
1:
    addi    s0, s0, 1
    j       1b
