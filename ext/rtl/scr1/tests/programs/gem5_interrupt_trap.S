    .section .text.trap, "ax"
    .align 2
    .global trap_entry
    .type trap_entry, @function
trap_entry:
    addi    sp, sp, -16
    sw      t0, 0(sp)
    sw      t1, 4(sp)

    csrr    t0, mcause
    li      t1, 0x80000003
    bne     t0, t1, 1f
    la      t0, interrupt_count
    lw      t1, 0(t0)
    addi    t1, t1, 1
    sw      t1, 0(t0)
    j       2f
1:
    la      t0, interrupt_count
    li      t1, -1
    sw      t1, 0(t0)
2:
    csrw    mie, zero
    lw      t0, 0(sp)
    lw      t1, 4(sp)
    addi    sp, sp, 16
    mret
    .size trap_entry, . - trap_entry
