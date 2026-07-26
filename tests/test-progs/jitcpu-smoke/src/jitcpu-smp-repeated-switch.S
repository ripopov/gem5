/* SPDX-License-Identifier: BSD-3-Clause */

    .equ NUM_HARTS,        4
    .equ LAST_SWITCH,      20
    .equ LINE_SIZE,        64
    .equ LINES_PER_HART,   1024
    .equ WORKING_SET_SIZE, (LINE_SIZE * LINES_PER_HART)
    .equ STACK_SIZE,       4096

    .section .text.init
    .option norvc
    .globl _start
_start:
    csrr    s1, mhartid
    li      t0, NUM_HARTS
    bgeu    s1, t0, fail_hart

    /* Give every hart private stack space before any switch can occur. */
    la      sp, stacks
    li      t0, STACK_SIZE
    addi    t1, s1, 1
    mul     t1, t1, t0
    add     sp, sp, t1

    la      s2, working_sets
    li      t0, WORKING_SET_SIZE
    mul     t1, s1, t0
    add     s2, s2, t1
    add     s3, s2, t0
    li      s0, 0

phase_loop:
    /* The release generation catches skipped or duplicated handoffs. */
    la      t0, release_phase
    ld      t1, 0(t0)
    bne     t1, s0, fail_phase

    beqz    s0, validate_initial_memory

    /* Callee-saved integer and FP state must survive on every hart. */
    addi    t0, s0, -1
    slli    t0, t0, 16
    xor     t0, t0, s1
    li      t1, 0x13579bdf00000000
    xor     t0, t0, t1
    bne     s8, t0, fail_integer

    addi    t0, s0, -1
    slli    t0, t0, 16
    xor     t0, t0, s1
    li      t1, 0x2468ace000000000
    xor     t0, t0, t1
    fmv.x.d t1, f8
    bne     t1, t0, fail_float

    /*
     * Validate the complete private footprint from the previous model.
     * Odd phases were written through O3/CHI and must be written back before
     * JIT reads them.  Even phases were written by JIT and must replace any
     * clean O3 copies on the following takeover.
     */
    mv      t0, s2
    li      t1, 0
validate_previous:
    addi    t2, s0, -1
    slli    t2, t2, 32
    slli    t3, s1, 24
    xor     t2, t2, t3
    xor     t2, t2, t1
    li      t3, 0x5a5aa5a500000000
    xor     t2, t2, t3
    ld      t3, 0(t0)
    bne     t3, t2, fail_memory
    addi    t0, t0, LINE_SIZE
    addi    t1, t1, 1
    li      t2, LINES_PER_HART
    bne     t1, t2, validate_previous
    j       write_current

validate_initial_memory:
    mv      t0, s2
validate_zero:
    ld      t1, 0(t0)
    bnez    t1, fail_memory
    addi    t0, t0, LINE_SIZE
    bne     t0, s3, validate_zero

write_current:
    mv      t0, s2
    li      t1, 0
write_line:
    slli    t2, s0, 32
    slli    t3, s1, 24
    xor     t2, t2, t3
    xor     t2, t2, t1
    li      t3, 0x5a5aa5a500000000
    xor     t2, t2, t3
    sd      t2, 0(t0)
    addi    t0, t0, LINE_SIZE
    addi    t1, t1, 1
    li      t2, LINES_PER_HART
    bne     t1, t2, write_line

    /* Exercise a cache line shared by all harts in every CPU model. */
    la      t0, shared_counter
    li      t1, 1
    amoadd.d.aqrl zero, t1, (t0)

    slli    t0, s0, 16
    xor     t0, t0, s1
    li      t1, 0x13579bdf00000000
    xor     s8, t0, t1
    slli    t0, s0, 16
    xor     t0, t0, s1
    li      t1, 0x2468ace000000000
    xor     t0, t0, t1
    fmv.d.x f8, t0

    /*
     * All four harts must finish before hart zero requests the handoff.
     * Keep arrivals monotonic so hart zero cannot reset the counter before a
     * secondary hart has observed the previous generation.
     */
    fence   rw, rw
    la      t0, arrived
    li      t1, 1
    amoadd.w.aqrl zero, t1, (t0)
wait_arrived:
    lw      t1, 0(t0)
    addi    t2, s0, 1
    li      t3, NUM_HARTS
    mul     t2, t2, t3
    bltu    t1, t2, wait_arrived
    fence   rw, rw
    bnez    s1, wait_release

    la      t0, shared_counter
    ld      t1, 0(t0)
    addi    t2, s0, 1
    li      t3, NUM_HARTS
    mul     t2, t2, t3
    bne     t1, t2, fail_atomic

    li      t0, LAST_SWITCH
    bgtu    s0, t0, finished

    /* M5OP_SWITCH_CPU (0x52). Execution resumes here in the other model. */
    .word   0xa400007b

    addi    t1, s0, 1
    fence   rw, rw
    la      t0, release_phase
    sd      t1, 0(t0)
    fence   rw, rw
    addi    s0, s0, 1
    j       phase_loop

wait_release:
    la      t0, release_phase
    addi    t2, s0, 1
1:
    ld      t1, 0(t0)
    bne     t1, t2, 1b
    fence   rw, rw
    addi    s0, s0, 1
    j       phase_loop

finished:
    li      a0, 0
    .word   0x4200007b       /* M5OP_EXIT (0x21). */
2:
    j       2b

fail_hart:
    li      a1, 30
    j       fail
fail_phase:
    li      a1, 31
    j       fail
fail_integer:
    li      a1, 32
    j       fail
fail_float:
    li      a1, 33
    j       fail
fail_memory:
    li      a1, 34
    j       fail
fail_atomic:
    li      a1, 35
fail:
    li      a0, 0
    .word   0x4400007b       /* M5OP_FAIL (0x22). */
3:
    j       3b

    .section .data
    .align 6
release_phase: .dword 0
shared_counter: .dword 0
arrived:       .word 0

    .section .bss
    .align 12
stacks:
    .skip (STACK_SIZE * NUM_HARTS)
    .align 12
working_sets:
    .skip (WORKING_SET_SIZE * NUM_HARTS)
