.section .text.init
.globl _start
_start:
    li sp, 0x02100000
    la t0, trap
    csrw mtvec, t0
    li t0, (1 << 7)
    csrw mie, t0
    li t0, (1 << 3)
    csrs mstatus, t0
    li t0, 0x01800018
    li t1, 1
    sd t1, 0(t0)
    fence iorw, iorw
first_idle:
    wfi
    j first_idle

.align 4
trap:
    csrwi mie, 0
    csrci mstatus, 8
    li t0, 0x01800018
    li t1, 2
    sd t1, 0(t0)
    fence iorw, iorw
second_idle:
    wfi
    j second_idle
