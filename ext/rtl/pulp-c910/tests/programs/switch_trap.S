.section .text.init
.globl _start
_start:
    la t0, trap_handler
    csrw mtvec, t0
    li t0, 0x1122334455667788
    csrw mscratch, t0
    li t0, 7
    csrw mcounteren, t0
    li t0, 0x01820300
    csrw mepc, t0
    li t0, 2
    csrw mcause, t0
    li t0, 0xa5
    csrw mtval, t0
    la t0, supervisor_vector
    csrw stvec, t0
    li t0, 0x8877665544332211
    csrw sscratch, t0
    li t0, 5
    csrw scounteren, t0
    li t0, 0x01820400
    csrw sepc, t0
    li t0, 2
    csrw scause, t0
    li t0, 0x55
    csrw stval, t0
    li t0, ((1 << 3) | (1 << 7) | (1 << 11))
    csrw mie, t0

    .word 0xa400007b

after_switch:
    li t5, 1
    la t0, trap_handler
    csrr t1, mtvec
    bne t0, t1, fail
    addi t5, t5, 1
    li t0, 0x1122334455667788
    csrr t1, mscratch
    bne t0, t1, fail
    addi t5, t5, 1
    li t0, 7
    csrr t1, mcounteren
    bne t0, t1, fail
    addi t5, t5, 1
    li t0, 0x01820300
    csrr t1, mepc
    bne t0, t1, fail
    addi t5, t5, 1
    li t0, 2
    csrr t1, mcause
    bne t0, t1, fail
    addi t5, t5, 1
    li t0, 0xa5
    csrr t1, mtval
    bne t0, t1, fail
    addi t5, t5, 1
    la t0, supervisor_vector
    csrr t1, stvec
    bne t0, t1, fail
    addi t5, t5, 1
    li t0, 0x8877665544332211
    csrr t1, sscratch
    bne t0, t1, fail
    addi t5, t5, 1
    li t0, 5
    csrr t1, scounteren
    bne t0, t1, fail
    addi t5, t5, 1
    li t0, 0x01820400
    csrr t1, sepc
    bne t0, t1, fail
    addi t5, t5, 1
    li t0, 2
    csrr t1, scause
    bne t0, t1, fail
    addi t5, t5, 1
    li t0, 0x55
    csrr t1, stval
    bne t0, t1, fail
    addi t5, t5, 1
    li t0, ((1 << 3) | (1 << 7) | (1 << 11))
    csrr t1, mie
    bne t0, t1, fail
ecall_site:
    ecall
    j fail

.align 4
trap_handler:
    li t5, 20
    csrr t0, mcause
    li t1, 11
    bne t0, t1, fail
    addi t5, t5, 1
    csrr t0, mepc
    la t1, ecall_site
    bne t0, t1, fail
    addi t5, t5, 1
    csrr t0, mtval
    bnez t0, fail
    li t0, 0x600dc91051000004
    j finish

supervisor_vector:
    j fail

fail:
    li t0, 0xbadc910510000004
    add t0, t0, t5
finish:
    li t1, 0x01820000
    sd t0, 0(t1)
    fence iorw, iorw
1:
    wfi
    j 1b
