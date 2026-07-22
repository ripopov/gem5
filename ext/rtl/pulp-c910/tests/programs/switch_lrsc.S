.section .text.init
.globl _start
_start:
    li t0, 0x10010000
    li t1, 100
    sd t1, 0(t0)
    li t1, 7
    sw t1, 16(t0)
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

    # OpenC910 represents an acquire-qualified SC as a local AxBAR ordering
    # transaction after the store. Exercise that exact path so the normalized
    # AXI4 port cannot regress to forwarding the barrier as an address-zero
    # write.
    addi t3, t0, 16
3:
    lr.w t1, (t3)
    addi t1, t1, 5
    sc.w.aq t2, t1, (t3)
    bnez t2, 3b
    lw t1, 0(t3)
    li t2, 12
    bne t1, t2, fail

    li t1, 0x600dc91051000008
    j finish
fail:
    li t1, 0xbadc910510000008
finish:
    li t0, 0x01820000
    sd t1, 0(t0)
    fence iorw, iorw
4:
    wfi
    j 4b
