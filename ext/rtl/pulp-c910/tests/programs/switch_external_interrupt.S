.section .text.init
.globl _start
_start:
    la t0, trap_handler
    csrw mtvec, t0
    .word 0xa400007b

after_switch:
    li t0, (1 << 11)
    csrw mie, t0
    li t0, (1 << 3)
    csrs mstatus, t0
1:
    wfi
    j 1b

.align 4
trap_handler:
    csrwi mie, 0
    csrci mstatus, 8
    csrr t0, mcause
    li t1, 0x800000000000000b
    bne t0, t1, fail
    li t0, 0x600dc91051000009
    j finish
fail:
    li t0, 0xbadc910510000009
finish:
    li t1, 0x01820000
    sd t0, 0(t1)
    fence iorw, iorw
2:
    wfi
    j 2b
