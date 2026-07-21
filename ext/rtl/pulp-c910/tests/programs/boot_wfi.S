.section .text.init
.globl _start
_start:
    li sp, 0x02100000
    li t0, 0x01800000
    li t1, 0x1122334455667788
    sd t1, 0(t0)
    ld t2, 0(t0)
    bne t1, t2, failed
    li t1, 0x600dc910
    sd t1, 8(t0)
    fence iorw, iorw
idle:
    wfi
    j idle
failed:
    li t1, 0xdeadc910
    sd t1, 8(t0)
    j idle
