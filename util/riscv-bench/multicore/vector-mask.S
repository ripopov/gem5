# SPDX-License-Identifier: BSD-3-Clause

# Exercise element indexing and preservation across an LMUL=8 register group.
# Only the first eight bytes are accessible; every other element is masked.
.section .text.init,"ax"
.globl _start
.option norvc
_start:
    csrr t0, mhartid
    bnez t0, park
    la t0, fail
    csrw mtvec, t0
    li t0, 0x6600
    csrs mstatus, t0
    la s0, data
    srli t0, s0, 2
    csrw pmpaddr0, t0
    ori t0, t0, 511
    csrw pmpaddr1, t0
    li t0, 0x989b
    csrw pmpcfg0, t0

    li s1, 256
    vsetvli zero, s1, e8, m8, tu, mu
    vmv.v.i v8, 9
    vmclr.m v0
    li t0, 255
    vmv.s.x v0, t0
#if CASE == 2
    vse8.v v8, (s0), v0.t
    li s1, 8
    li t2, 9
    mv t0, s0
check_store:
    lbu t1, (t0)
    bne t1, t2, fail
    addi t0, t0, 1
    addi s1, s1, -1
    bnez s1, check_store
#else
    vle8.v v8, (s0), v0.t
    la t0, result
    vse8.v v8, (t0)
    li t3, 0
check_load:
    li t2, 17
    li t4, 8
    bltu t3, t4, 1f
    li t2, 9
1:  lbu t1, (t0)
    bne t1, t2, fail
    addi t0, t0, 1
    addi t3, t3, 1
    bne t3, s1, check_load
#endif
    li a0, 0
    .word 0x4200007b
park: j park
.balign 4
fail:
    li a0, 0
    li a1, 80
    .word 0x4400007b
    j fail
.section .data
.balign 4096
data: .fill 8, 1, 17
.space 4088
result: .space 256
