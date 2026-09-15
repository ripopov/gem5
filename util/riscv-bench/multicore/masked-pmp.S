# SPDX-License-Identifier: BSD-3-Clause

# Only element zero is accessible. Masked elements must neither issue
# accesses nor overwrite an undisturbed destination register.
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
    csrw pmpaddr0, t0 # NAPOT, 8 bytes
    ori t0, t0, 511
    csrw pmpaddr1, t0 # Rest of the page is inaccessible, including M-mode.
    li t0, 0x989b
    csrw pmpcfg0, t0
    li s4, 0
again:
#if CASE == 5
    vsetivli zero, 0, e64, m1, tu, mu
    vle64ff.v v1, (s0)
    csrr t0, vl
    bnez t0, fail
    j pass
#endif
    vsetivli zero, 4, e64, m1, tu, mu
    vmv.v.i v1, 9
    li t0, 1
#if CASE == 2
    bnez s4, 1f
    li t0, 5 # Element two will fault, suppressing the trap and trimming VL.
1:
#endif
    vmv.s.x v0, t0
#if CASE == 3
    vse64.v v1, (s0), v0.t
    ld t0, (s0)
    li t1, 9
    bne t0, t1, fail
    j pass
#elif CASE == 4
    vle64ff.v v1, (s0)
    csrr t0, vl
    li t1, 1
    bne t0, t1, fail
#elif CASE == 2
    vle64ff.v v1, (s0), v0.t
    csrr t0, vl
    li t1, 2
    beqz s4, 2f
    li t1, 4
2:  bne t0, t1, fail
#else
    vle64.v v1, (s0), v0.t
#endif
    vsetivli zero, 4, e64, m1, tu, mu
    la t0, result
    vse64.v v1, (t0)
    ld t1, 0(t0)
    li t2, 17
    bne t1, t2, fail
    ld t1, 8(t0)
    li t2, 9
    bne t1, t2, fail
    ld t1, 24(t0)
    bne t1, t2, fail
    addi s4, s4, 1
    li t0, 2
    bne s4, t0, again
pass:
    li a0, 0
    .word 0x4200007b
park: j park
.balign 4
fail:
    li a0, 0
    li a1, 79
    .word 0x4400007b
    j fail
.section .data
.balign 4096
data: .quad 17
.space 4088
result: .space 32
