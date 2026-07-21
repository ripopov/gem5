    .section .text
    .globl _start
_start:
    li t0, 0x10000000
    li t1, 'J'
    sb t1, 0(t0)
    li t1, 'I'
    sb t1, 0(t0)
    li t1, 'T'
    sb t1, 0(t0)
    li t1, '\n'
    sb t1, 0(t0)

    /* M5OP_SWITCH_CPU (0x52). */
    .word 0xa400007b

    li t1, 'O'
    sb t1, 0(t0)
    li t1, '3'
    sb t1, 0(t0)
    li t1, '\n'
    sb t1, 0(t0)

    li a0, 0
    /* M5OP_EXIT (0x21). */
    .word 0x4200007b
1:
    j 1b
