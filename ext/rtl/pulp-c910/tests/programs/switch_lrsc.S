.section .text.init
.globl _start
_start:
    li t0, 0x10010000
    li t1, 100
    sd t1, 0(t0)
    .word 0xa400007b

after_switch:
    # OpenC910 requires its memory-attribute extension and data cache for
    # LR/SC. These are implementation CSRs, so enable them only after
    # the generic RISC-V CPU has switched to the RTL core.
    li t6, 0x8000
    csrs 0x7c0, t6
    csrsi 0x7c1, 0x2

    li t0, 0x10010000
1:
    lr.d.aq t1, (t0)
    addi t1, t1, 23
    sc.d.rl t2, t1, (t0)
    bnez t2, 1b
    ld t1, 0(t0)
    li t2, 123
    bne t1, t2, fail
    li t1, 0x600dc91051000008
    j finish
fail:
    li t1, 0xbadc910510000008
finish:
    li t0, 0x01820000
    sd t1, 0(t0)
    fence iorw, iorw
2:
    wfi
    j 2b
