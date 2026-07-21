.section .text.init
.globl _start
_start:
    li sp, 0x02100000
    la t0, trap
    csrw mtvec, t0
    li t0, 0x01801000
    ld t1, 0(t0)
    li t0, 0x01800010
    li t1, 0xbad0
    sd t1, 0(t0)
idle:
    wfi
    j idle

.align 4
trap:
    li t0, 0x01800010
    li t1, 0xe220
    sd t1, 0(t0)
    j idle
