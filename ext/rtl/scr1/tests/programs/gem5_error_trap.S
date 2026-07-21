/* Validate that a failed AXI read reaches SCR1 as a load access fault. */

    .section .text
    .align 2
    .global gem5_trigger_bad_load
    .type gem5_trigger_bad_load, @function
gem5_trigger_bad_load:
    .option push
    .option norvc
    lw      a0, 0(a0)
    .option pop
    ret
    .size gem5_trigger_bad_load, . - gem5_trigger_bad_load

    .align 2
    .global gem5_error_trap
    .type gem5_error_trap, @function
gem5_error_trap:
    csrr    t0, mcause
    li      t1, 5
    bne     t0, t1, 1f

    la      t0, error_seen
    li      t1, 1
    sw      t1, 0(t0)
    csrr    t0, mepc
    addi    t0, t0, 4
    csrw    mepc, t0
    mret

1:
    j       1b
    .size gem5_error_trap, . - gem5_error_trap
