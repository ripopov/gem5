/* SPDX-License-Identifier: BSD-3-Clause */

    .equ NUM_HARTS,        16
    .equ UART,             0x10000000
    .equ LINE_SIZE,        64
    .equ LINES_PER_HART,   512
    .equ WORKING_SET_SIZE, (LINE_SIZE * LINES_PER_HART)
    .equ STACK_SIZE,       4096

    .section .text.init
    .option norvc
    .globl _start
_start:
    csrr    s0, mhartid
    li      t0, NUM_HARTS
    bgeu    s0, t0, fail_hart

    la      sp, stacks
    li      t0, STACK_SIZE
    addi    t1, s0, 1
    mul     t1, t1, t0
    add     sp, sp, t1

    la      s1, working_sets
    li      t0, WORKING_SET_SIZE
    mul     t1, s0, t0
    add     s1, s1, t1
    add     s2, s1, t0

    li      t0, 0x13579bdf00000000
    xor     s8, t0, s0
    li      t0, 0x2468ace000000000
    xor     t0, t0, s0
    fmv.d.x f8, t0

    /* Every JitCPU hart writes a private cold footprint. */
    mv      t0, s1
    li      t1, 0
jit_write:
    slli    t2, s0, 32
    xor     t2, t2, t1
    li      t3, 0x5a5aa5a500000000
    xor     t2, t2, t3
    sd      t2, 0(t0)
    addi    t0, t0, LINE_SIZE
    addi    t1, t1, 1
    li      t2, LINES_PER_HART
    bne     t1, t2, jit_write

    la      t0, shared_counter
    li      t1, 1
    amoadd.d.aqrl zero, t1, (t0)
    fence   rw, rw

    la      t0, jit_arrived
    amoadd.w.aqrl zero, t1, (t0)
wait_for_jit:
    lw      t2, 0(t0)
    li      t3, NUM_HARTS
    bne     t2, t3, wait_for_jit
    fence   rw, rw

    bnez    s0, wait_for_o3
    la      t0, shared_counter
    ld      t1, 0(t0)
    li      t2, NUM_HARTS
    bne     t1, t2, fail_atomic
    la      a0, jit_ready_message
    call    uart_puts

    /* The host drains and switches all 16 CPU objects as one operation. */
    .word   0xa400007b       /* M5OP_SWITCH_CPU (0x52). */

    fence   rw, rw
    la      t0, o3_release
    li      t1, 1
    sw      t1, 0(t0)
    fence   rw, rw
    j       o3_start

wait_for_o3:
    la      t0, o3_release
1:
    lw      t1, 0(t0)
    beqz    t1, 1b
    fence   rw, rw

o3_start:
    /* Callee-saved integer and floating-point state crossed the handoff. */
    li      t0, 0x13579bdf00000000
    xor     t0, t0, s0
    bne     s8, t0, fail_integer
    li      t0, 0x2468ace000000000
    xor     t0, t0, s0
    fmv.x.d t1, f8
    bne     t1, t0, fail_float

    /* O3 must fetch every line from memory through its RNF and an SNF. */
    mv      t0, s1
    li      t1, 0
o3_verify:
    slli    t2, s0, 32
    xor     t2, t2, t1
    li      t3, 0x5a5aa5a500000000
    xor     t2, t2, t3
    ld      t3, 0(t0)
    bne     t3, t2, fail_memory
    not     t2, t2
    sd      t2, 0(t0)
    addi    t0, t0, LINE_SIZE
    addi    t1, t1, 1
    li      t2, LINES_PER_HART
    bne     t1, t2, o3_verify

    la      t0, shared_counter
    li      t1, 1
    amoadd.d.aqrl zero, t1, (t0)
    fence   rw, rw

    la      t0, o3_arrived
    amoadd.w.aqrl zero, t1, (t0)
wait_for_o3_done:
    lw      t2, 0(t0)
    li      t3, NUM_HARTS
    bne     t2, t3, wait_for_o3_done
    fence   rw, rw
    bnez    s0, parked

    la      t0, shared_counter
    ld      t1, 0(t0)
    li      t2, (NUM_HARTS * 2)
    bne     t1, t2, fail_atomic
    la      a0, o3_pass_message
    call    uart_puts
    li      a0, 0
    .word   0x4200007b       /* M5OP_EXIT (0x21). */

parked:
2:
    j       2b

uart_puts:
    li      t0, UART
3:
    lbu     t1, 0(a0)
    beqz    t1, 4f
    sb      t1, 0(t0)
    addi    a0, a0, 1
    j       3b
4:
    ret

fail_hart:
    li      a1, 60
    j       fail
fail_integer:
    li      a1, 61
    j       fail
fail_float:
    li      a1, 62
    j       fail
fail_memory:
    li      a1, 63
    j       fail
fail_atomic:
    li      a1, 64
fail:
    li      a0, 0
    .word   0x4400007b       /* M5OP_FAIL (0x22). */
5:
    j       5b

    .section .rodata
jit_ready_message: .asciz "JITCPU-MESH16 READY\n"
o3_pass_message:   .asciz "JITCPU-MESH16 PASS\n"

    .section .data
    .align 6
shared_counter: .dword 0
jit_arrived:    .word 0
o3_release:     .word 0
o3_arrived:     .word 0

    .section .bss
    .align 12
stacks:
    .skip (STACK_SIZE * NUM_HARTS)
    .align 12
working_sets:
    .skip (WORKING_SET_SIZE * NUM_HARTS)
