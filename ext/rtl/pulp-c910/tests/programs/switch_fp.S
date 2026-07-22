.section .text.init
.globl _start

.macro load_f index
    li t0, (0x3ff0000000000000 + \index)
    fmv.d.x f\index, t0
.endm

.macro dump_f index
    fsd f\index, (8 * \index)(t6)
.endm

_start:
    .irp index,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        load_f \index
    .endr
    .irp index,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
        load_f \index
    .endr
    li t0, 0x61
    csrw fcsr, t0

    .word 0xa400007b

    li t6, 0x01820000
    .irp index,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        dump_f \index
    .endr
    .irp index,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
        dump_f \index
    .endr
    csrr t0, fcsr
    sd t0, 256(t6)

    li t0, 0x3ff8000000000000
    fmv.d.x f0, t0
    li t0, 0x4002000000000000
    fmv.d.x f1, t0
    fadd.d f2, f0, f1
    fmv.x.d t1, f2
    sd t1, 264(t6)
    fmul.d f2, f0, f1
    fmv.x.d t1, f2
    sd t1, 272(t6)
    li t0, 0x600dc91051000003
    sd t0, 280(t6)
    fence iorw, iorw
1:
    wfi
    j 1b
