# SPDX-License-Identifier: BSD-3-Clause

.section .text.init,"ax"
.globl _start
.option norvc
_start:
    csrr t0, mhartid
    li t1, 1
    bgtu t0, t1, park
    la s0, data
    li s1, 0x2000000
    bnez t0, sender
    la t0, trap
    csrw mtvec, t0
#ifdef SWITCH
    li a0, 0
    li a1, 0
    .word 0xb400007b
#endif
#if CASE == 2
    li t0, 128
#else
    li t0, 8
#endif
    csrs mie, t0
#if CASE != 3
    li t0, 8
    csrs mstatus, t0
#endif
    li t0, 41
    sd t0, 0(s0)
    lr.d t0, (s0)
#if CASE == 2
    li t0, 0xbff8
    add t0, t0, s1
    ld t1, 0(t0)
    addi t1, t1, 20
    li t0, 0x4000
    add t0, t0, s1
    sd t1, 0(t0)
#else
    fence rw,rw
    li t0, 1
    sd t0, 64(s0)
#endif
wait_irq:
    wfi
#if CASE == 3
    csrr t0, mip
    andi t0, t0, 8
#else
    ld t0, 128(s0)
#endif
    beqz t0, wait_irq
#if CASE != 2
    fence rw,rw
    ld t0, 0(s0)
    li t1, 42
    bne t0, t1, fail
    sc.d t0, zero, (s0)
    beqz t0, fail
#endif
    li a0, 0
    .word 0x4200007b
sender:
#if CASE != 2
    ld t0, 64(s0)
    beqz t0, sender
    li t0, 1000
1:  addi t0, t0, -1
    bnez t0, 1b
    li t0, 42
    sd t0, 0(s0)
    fence rw,rw
    li t0, 1
    sw t0, 0(s1)
#endif
park: j park
trap:
    csrr t3, mcause
#if CASE == 2
    li t4, 0x8000000000000007
#else
    li t4, 0x8000000000000003
#endif
    bne t3, t4, fail
#if CASE == 2
    li t3, 0x4000
    add t3, t3, s1
    li t4, -1
    sd t4, 0(t3)
#else
    sw zero, 0(s1)
#endif
    li t3, 1
    sd t3, 128(s0)
    mret
fail:
    li a0, 0
    li a1, 74
    .word 0x4400007b
    j fail
.section .data
.balign 64
data: .space 192
