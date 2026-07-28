/* SPDX-License-Identifier: BSD-3-Clause */

#ifndef JITCPU_NUM_HARTS
#define JITCPU_NUM_HARTS 4
#endif
#ifndef JITCPU_LINES_PER_HART
#define JITCPU_LINES_PER_HART 1024
#endif

    .equ NUM_HARTS,        JITCPU_NUM_HARTS
    .equ LAST_SWITCH,      21
    .equ CLINT_MSIP,       0x02000000
    .equ LINE_SIZE,        64
    .equ LINES_PER_HART,   JITCPU_LINES_PER_HART
    .equ WORKING_SET_SIZE, (LINE_SIZE * LINES_PER_HART)
    .equ STACK_SIZE,       4096
    .equ M5_SUM_RESULT,    0x1234
    .equ WFI_SENTINEL,     0x5eed

    .section .text.init
    .option norvc
    .globl _start
_start:
    csrr    s1, mhartid
    li      t0, NUM_HARTS
    bgeu    s1, t0, fail_hart

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

    li      t0, 0x13579bdf00000000
    xor     s8, t0, s1
    li      t0, 0x2468ace000000000
    xor     t0, t0, s1
    fmv.d.x f8, t0

    li      s0, 0
    call    touch_phase

    beqz    s1, driver_start

    la      t0, trap_vector
    csrw    mtvec, t0
    li      t0, 8
    csrs    mie, t0
    csrs    mstatus, t0

    fence   rw, rw
    la      t0, ready_harts
    li      t1, 1
    amoadd.w.aqrl zero, t1, (t0)

secondary_wait:
    /*
     * Suspend immediately after a gem5 pseudo instruction. Both are reported
     * to the JIT backend through the same QEMU EXCP_HLT exit, so a hart that
     * has just executed one must still be seen to suspend: the pseudo
     * instruction must not be replayed as the reason for this WFI.
     */
    call    check_m5_sum
    li      a0, WFI_SENTINEL
    wfi
    j       8f
    /* A replayed pseudo instruction retires the WFI and skips the branch. */
    j       fail_wfi_replay
8:
    li      t0, WFI_SENTINEL
    bne     a0, t0, fail_wfi_replay

    la      t0, release_phase
    ld      t1, 0(t0)
    addi    t2, s0, 1
    /* RISC-V permits a WFI to resume without an interrupt. */
    bltu    t1, t2, secondary_wait
    bne     t1, t2, fail_phase
    addi    s0, s0, 1
    call    check_state
    call    touch_phase

    /* A spurious WFI return cannot replace the expected CLINT interrupt. */
    la      t0, interrupt_counts
    slli    t1, s1, 3
    add     t0, t0, t1
6:
    ld      t1, 0(t0)
    bltu    t1, s0, 6b
    bne     t1, s0, fail_trap

    fence   rw, rw
    la      t0, arrived_harts
    li      t1, 1
    amoadd.w.aqrl zero, t1, (t0)
    j       secondary_wait

driver_start:
    la      t0, ready_harts
1:
    lw      t1, 0(t0)
    li      t2, NUM_HARTS - 1
    bne     t1, t2, 1b
    fence   rw, rw
    call    wait_for_wfi

    la      t0, shared_counter
    ld      t1, 0(t0)
    li      t2, NUM_HARTS
    bne     t1, t2, fail_atomic

driver_switch:
    li      t0, LAST_SWITCH
    bgeu    s0, t0, finished

    /* All secondary harts are suspended in WFI at this handoff. */
    .word   0xa400007b       /* M5OP_SWITCH_CPU (0x52). */

    addi    s0, s0, 1
    call    check_state
    call    touch_phase

    fence   rw, rw
    la      t0, release_phase
    sd      s0, 0(t0)
    fence   rw, rw

    /* Wake each suspended hart through its own CLINT MSIP register. */
    li      t0, CLINT_MSIP + 4
    li      t1, 1
    li      t2, NUM_HARTS - 1
7:
    sw      t1, 0(t0)
    addi    t0, t0, 4
    addi    t2, t2, -1
    bnez    t2, 7b

    la      t0, arrived_harts
2:
    lw      t1, 0(t0)
    li      t2, NUM_HARTS - 1
    bne     t1, t2, 2b
    fence   rw, rw

    la      t0, shared_counter
    ld      t1, 0(t0)
    addi    t2, s0, 1
    li      t3, NUM_HARTS
    mul     t2, t2, t3
    bne     t1, t2, fail_atomic

    la      t0, arrived_harts
    sw      zero, 0(t0)
    fence   rw, rw
    call    wait_for_wfi
    j       driver_switch

finished:
    li      a0, 0
    .word   0x4200007b       /* M5OP_EXIT (0x21). */
3:
    j       3b

/* Validate the state that must cross each suspended or active takeover. */
check_state:
    li      t0, 0x13579bdf00000000
    xor     t0, t0, s1
    bne     s8, t0, fail_integer

    li      t0, 0x2468ace000000000
    xor     t0, t0, s1
    fmv.x.d t1, f8
    bne     t1, t0, fail_float
    ret

/*
 * M5OP_SUM is the gem5 pseudo-instruction sanity check: it returns the sum of
 * its first six arguments. Both CPU models must implement it identically.
 */
check_m5_sum:
    li      a0, 0x1000
    li      a1, 0x200
    li      a2, 0x30
    li      a3, 4
    li      a4, 0
    li      a5, 0
    .word   0x4600007b       /* M5OP_SUM (0x23). */
    li      t0, M5_SUM_RESULT
    bne     a0, t0, fail_m5op
    ret

/* Let secondaries retire their post-arrival branch and enter WFI. */
wait_for_wfi:
    li      t0, 16384
5:
    addi    t0, t0, -1
    bnez    t0, 5b
    ret

/* Validate the preceding model's footprint, then write this phase's value. */
touch_phase:
    beqz    s0, write_current
    mv      t0, s2
    li      t1, 0
validate_previous:
    addi    t2, s0, -1
    slli    t2, t2, 32
    slli    t3, s1, 24
    xor     t2, t2, t3
    xor     t2, t2, t1
    li      t3, 0xa55a5aa500000000
    xor     t2, t2, t3
    ld      t3, 0(t0)
    bne     t3, t2, fail_memory
    addi    t0, t0, LINE_SIZE
    addi    t1, t1, 1
    li      t2, LINES_PER_HART
    bne     t1, t2, validate_previous

write_current:
    mv      t0, s2
    li      t1, 0
write_line:
    slli    t2, s0, 32
    slli    t3, s1, 24
    xor     t2, t2, t3
    xor     t2, t2, t1
    li      t3, 0xa55a5aa500000000
    xor     t2, t2, t3
    sd      t2, 0(t0)
    addi    t0, t0, LINE_SIZE
    addi    t1, t1, 1
    li      t2, LINES_PER_HART
    bne     t1, t2, write_line

    la      t0, shared_counter
    li      t1, 1
    amoadd.d.aqrl zero, t1, (t0)
    ret

    .align 2
trap_vector:
    csrr    t0, mcause
    bgez    t0, fail_trap
    andi    t0, t0, 0xff
    li      t1, 3
    bne     t0, t1, fail_trap

    csrr    t0, mhartid
    la      t1, interrupt_counts
    slli    t2, t0, 3
    add     t1, t1, t2
    ld      t2, 0(t1)
    addi    t2, t2, 1
    sd      t2, 0(t1)

    slli    t0, t0, 2
    li      t1, CLINT_MSIP
    add     t0, t0, t1
    sw      zero, 0(t0)
    mret

fail_hart:
    li      a1, 40
    j       fail
fail_phase:
    li      a1, 41
    j       fail
fail_integer:
    li      a1, 42
    j       fail
fail_float:
    li      a1, 43
    j       fail
fail_memory:
    li      a1, 44
    j       fail
fail_atomic:
    li      a1, 45
    j       fail
fail_trap:
    li      a1, 46
    j       fail
fail_m5op:
    li      a1, 47
    j       fail
fail_wfi_replay:
    li      a1, 48
fail:
    li      a0, 0
    .word   0x4400007b       /* M5OP_FAIL (0x22). */
4:
    j       4b

    .section .data
    .align 6
release_phase:  .dword 0
shared_counter: .dword 0
ready_harts:    .word 0
arrived_harts:  .word 0
    .align 3
interrupt_counts:
    .skip (8 * NUM_HARTS)

    .section .bss
    .align 12
stacks:
    .skip (STACK_SIZE * NUM_HARTS)
    .align 12
working_sets:
    .skip (WORKING_SET_SIZE * NUM_HARTS)
