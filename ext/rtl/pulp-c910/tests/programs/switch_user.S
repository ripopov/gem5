.section .text.init
.globl _start
_start:
    la t0, machine_trap
    csrw mtvec, t0

    # Grant U-mode access to the complete physical address space.
    li t0, 0x3fffffffff
    csrw pmpaddr0, t0
    li t0, 0x0f
    csrw pmpcfg0, t0

    csrr t0, mstatus
    li t1, ~(3 << 11)
    and t0, t0, t1
    csrw mstatus, t0
    la t0, user_start
    csrw mepc, t0
    mret

user_start:
    li t0, 0x01820180
    li t1, 0x0123456789abcdef
    sd t1, 0(t0)
    .word 0xa400007b

after_switch:
    li t0, 0x01820180
    ld t1, 0(t0)
    li t2, 0x0123456789abcdef
    bne t1, t2, user_fail
ecall_site:
    ecall
    j user_fail

user_fail:
    li t0, 0xbadc91051000000a
    j finish

.align 4
machine_trap:
    csrr t0, mcause
    li t1, 8
    bne t0, t1, trap_fail
    csrr t0, mepc
    la t1, ecall_site
    bne t0, t1, trap_fail
    li t0, 0x600dc9105100000a
    j finish

trap_fail:
    li t0, 0xbadc91052000000a
finish:
    li t1, 0x01820000
    sd t0, 0(t1)
    fence iorw, iorw
1:
    wfi
    j 1b
