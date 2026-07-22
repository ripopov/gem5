.section .text.init
.globl _start
_start:
    wfi
    j _start

.section .text.resume, "ax"
.balign 16
.globl state_import_resume
state_import_resume:
    sd x0,   0(x31)
    sd x1,   8(x31)
    sd x2,  16(x31)
    sd x3,  24(x31)
    sd x4,  32(x31)
    sd x5,  40(x31)
    sd x6,  48(x31)
    sd x7,  56(x31)
    sd x8,  64(x31)
    sd x9,  72(x31)
    sd x10, 80(x31)
    sd x11, 88(x31)
    sd x12, 96(x31)
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

    fsd f0,  256(x31)
    fsd f1,  264(x31)
    fsd f2,  272(x31)
    fsd f3,  280(x31)
    fsd f4,  288(x31)
    fsd f5,  296(x31)
    fsd f6,  304(x31)
    fsd f7,  312(x31)
    fsd f8,  320(x31)
    fsd f9,  328(x31)
    fsd f10, 336(x31)
    fsd f11, 344(x31)
    fsd f12, 352(x31)
    fsd f13, 360(x31)
    fsd f14, 368(x31)
    fsd f15, 376(x31)
    fsd f16, 384(x31)
    fsd f17, 392(x31)
    fsd f18, 400(x31)
    fsd f19, 408(x31)
    fsd f20, 416(x31)
    fsd f21, 424(x31)
    fsd f22, 432(x31)
    fsd f23, 440(x31)
    fsd f24, 448(x31)
    fsd f25, 456(x31)
    fsd f26, 464(x31)
    fsd f27, 472(x31)
    fsd f28, 480(x31)
    fsd f29, 488(x31)
    fsd f30, 496(x31)
    fsd f31, 504(x31)

    li x1, 0x600dc9105a7e0001
    sd x1, 512(x31)
1:
    wfi
    j 1b
