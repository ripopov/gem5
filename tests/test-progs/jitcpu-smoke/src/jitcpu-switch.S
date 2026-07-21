/* SPDX-License-Identifier: BSD-3-Clause */

    .section .text
    .globl _start
_start:
    /*
     * Install an all-memory PMP rule and enter S-mode before switching.
     * The destination CPU must transfer the decoded PMP table as well as
     * the visible CSRs, or its first S-mode fetch will trap to pmp_fault.
     */
    la t0, pmp_fault
    csrw mtvec, t0
    li t0, 0x3fffffffffffff
    csrw pmpaddr0, t0
    li t0, 0x1f
    csrw pmpcfg0, t0

    la t0, supervisor_start
    csrw mepc, t0
    csrr t1, mstatus
    li t2, ~(3 << 11)
    and t1, t1, t2
    li t2, 1 << 11
    or t1, t1, t2
    csrw mstatus, t1
    mret

supervisor_start:
    li t0, 0x10000000
    li t1, 'J'
    sb t1, 0(t0)
    li t1, 'I'
    sb t1, 0(t0)
    li t1, 'T'
    sb t1, 0(t0)
    li t1, '\n'
    sb t1, 0(t0)

    /* M5OP_SWITCH_CPU (0x52). */
    .word 0xa400007b

    li t1, 'O'
    sb t1, 0(t0)
    li t1, '3'
    sb t1, 0(t0)
    li t1, '\n'
    sb t1, 0(t0)

    li a0, 0
    /* M5OP_EXIT (0x21). */
    .word 0x4200007b
1:
    j 1b

    .align 2
pmp_fault:
    li t0, 0x10000000
    li t1, 'P'
    sb t1, 0(t0)
    li t1, 'M'
    sb t1, 0(t0)
    li t1, 'P'
    sb t1, 0(t0)
    li t1, '\n'
    sb t1, 0(t0)

    li a0, 0
    li a1, 1
    /* M5OP_FAIL (0x22). */
    .word 0x4400007b
2:
    j 2b
