/* SPDX-License-Identifier: BSD-3-Clause */

    .section .text.init
    .option norvc
    .globl _start
_start:
    /* Linux reaches deterministic userspace before host-driven switching. */
    li      a0, 0
    .word   0x4200007b       /* M5OP_EXIT (0x21). */

    /* Keep PID 1 alive and generate sustained user-mode memory traffic. */
    la      s0, working_set
    li      s1, (256 * 1024)
    li      s2, 1
1:
    mv      t0, s0
    add     t1, s0, s1
2:
    ld      t2, 0(t0)
    add     t2, t2, s2
    sd      t2, 0(t0)
    addi    t0, t0, 64
    bne     t0, t1, 2b
    addi    s2, s2, 1
    fence   rw, rw
    j       1b

    .section .bss
    .align 12
working_set:
    .skip (256 * 1024)
