# SPDX-License-Identifier: BSD-3-Clause

.section .text.init,"ax"
.globl _start
.option norvc
_start:
    csrr t0, mhartid
    bnez t0, park
#ifdef ZERO
    li s0, 0
#else
    la s0, data
#endif
    lr.d t0, (s0)
#ifndef ZERO
    li a0, 0
    li a1, 0
    .word 0xb400007b
    # Consume a fresh reservation on the replacement CPU.
1:  lr.d t0, (s0)
    sc.d t0, zero, (s0)
    bnez t0, 1b
    lr.d t0, (s0)
#endif
    addi t1, s0, 64
    sc.d t0, zero, (t1)  # consumes the hart's reservation, despite failing
    beqz t0, fail
    li a0, 0
    li a1, 0
#ifdef CHECKPOINT
    .word 0x8600007b
#else
    .word 0xb400007b
#endif
    sc.d t0, zero, (s0)  # no LR since the preceding SC: must fail
    beqz t0, fail
    li a0, 0
    .word 0x4200007b
park: j park
fail:
    li a0, 0
    li a1, 72
    .word 0x4400007b
    j fail
.section .data
.balign 64
data: .space 128
