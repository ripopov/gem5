.section .text.init
.globl _start
_start:
    li t0, 0x01840000
    li t1, 512
1:
    sd zero, 0(t0)
    addi t0, t0, 8
    addi t1, t1, -1
    bnez t1, 1b

    # Two level-2 gigapage leaves map VA 0 and VA 0x40000000 to PA 0.
    li t0, 0x01840000
    li t1, 0xcf
    sd t1, 0(t0)
    sd t1, 8(t0)

    li t0, 0x3fffffffff
    csrw pmpaddr0, t0
    li t0, 0x0f
    csrw pmpcfg0, t0
    li t0, 0x8000000000001840
    csrw satp, t0
    sfence.vma zero, zero

    csrr t0, mstatus
    li t1, ~(3 << 11)
    and t0, t0, t1
    li t1, (1 << 11)
    or t0, t0, t1
    csrw mstatus, t0
    la t0, supervisor_start
    csrw mepc, t0
    mret

supervisor_start:
    li t0, 0x41820100
    li t1, 0x0123456789abcdef
    sd t1, 0(t0)
    .word 0xa400007b

after_switch:
    li t0, 0x8000000000001840
    csrr t1, satp
    bne t0, t1, fail
    li t0, 0x41820100
    ld t1, 0(t0)
    li t2, 0x0123456789abcdef
    bne t1, t2, fail
    li t0, 0x41820000
    li t1, 0x600dc91051000005
    sd t1, 0(t0)
    fence iorw, iorw
1:
    wfi
    j 1b

fail:
    li t0, 0x41820000
    li t1, 0xbadc910510000005
    sd t1, 0(t0)
    fence iorw, iorw
2:
    wfi
    j 2b
