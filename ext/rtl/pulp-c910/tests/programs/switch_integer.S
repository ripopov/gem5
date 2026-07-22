.section .text.init
.globl _start
_start:
    li x1,  0x1111000000000001
    li x2,  0x1111000000000002
    li x3,  0x1111000000000003
    li x4,  0x1111000000000004
    li x5,  0x1111000000000005
    li x6,  0x1111000000000006
    li x7,  0x1111000000000007
    li x8,  0x1111000000000008
    li x9,  0x1111000000000009
    li x10, 0x111100000000000a
    li x11, 0x111100000000000b
    li x12, 0x111100000000000c
    li x13, 0x111100000000000d
    li x14, 0x111100000000000e
    li x15, 0x111100000000000f
    li x16, 0x1111000000000010
    li x17, 0x1111000000000011
    li x18, 0x1111000000000012
    li x19, 0x1111000000000013
    li x20, 0x1111000000000014
    li x21, 0x1111000000000015
    li x22, 0x1111000000000016
    li x23, 0x1111000000000017
    li x24, 0x1111000000000018
    li x25, 0x1111000000000019
    li x26, 0x111100000000001a
    li x27, 0x111100000000001b
    li x28, 0x111100000000001c
    li x29, 0x111100000000001d
    li x30, 0x111100000000001e
    li x31, 0x01820000

    # m5_switch_cpu. The simulation exits here on AtomicSimpleCPU; the C910
    # must resume at the immediately following instruction with every GPR.
    .word 0xa400007b

    sd x0,    0(x31)
    sd x1,    8(x31)
    sd x2,   16(x31)
    sd x3,   24(x31)
    sd x4,   32(x31)
    sd x5,   40(x31)
    sd x6,   48(x31)
    sd x7,   56(x31)
    sd x8,   64(x31)
    sd x9,   72(x31)
    sd x10,  80(x31)
    sd x11,  88(x31)
    sd x12,  96(x31)
    sd x13, 104(x31)
    sd x14, 112(x31)
    sd x15, 120(x31)
    sd x16, 128(x31)
    sd x17, 136(x31)
    sd x18, 144(x31)
    sd x19, 152(x31)
    sd x20, 160(x31)
    sd x21, 168(x31)
    sd x22, 176(x31)
    sd x23, 184(x31)
    sd x24, 192(x31)
    sd x25, 200(x31)
    sd x26, 208(x31)
    sd x27, 216(x31)
    sd x28, 224(x31)
    sd x29, 232(x31)
    sd x30, 240(x31)
    sd x31, 248(x31)
    li x1, 0x600dc91051000001
    sd x1, 256(x31)
    fence iorw, iorw
1:
    wfi
    j 1b
