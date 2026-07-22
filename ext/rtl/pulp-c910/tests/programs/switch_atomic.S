.section .text.init
.globl _start
_start:
    la t5, trap_handler
    csrw mtvec, t5
    li t0, 0x10010000
    li t1, 41
    sd t1, 0(t0)
    li t1, 1
    amoadd.d.aqrl t2, t1, (t0)
    li t3, 41
    bne t2, t3, fail
    ld t2, 0(t0)
    li t3, 42
    bne t2, t3, fail
    li t1, 100
    sd t1, 8(t0)
    .word 0xa400007b

after_switch:
    # The first AMO proves visibility of a fast-CPU atomic write. Execute a
    # second AMO on RTL to cover both sides of the handover. Enable the C910
    # memory mode and data cache before accessing C910's cacheable PMA window.
    li t6, 0x8000
    csrs 0x7c0, t6
    csrsi 0x7c1, 0x2
    fence iorw, iorw

    li t0, 0x10010000
    ld t2, 0(t0)
    li t3, 42
    bne t2, t3, fail

    li t1, 23
    addi t4, t0, 8
    amoadd.d t2, t1, (t4)
    li t3, 100
    bne t2, t3, fail
    ld t2, 8(t0)
    li t3, 123
    bne t2, t3, fail
    li t1, 0x600dc91051000006
    j finish

fail:
    li t1, 0xbadc910510000006
    j finish

.align 4
trap_handler:
    li t1, 0xbadc910520000000
    csrr t2, mcause
    add t1, t1, t2
finish:
    li t0, 0x01820000
    sd t1, 0(t0)
    fence iorw, iorw
2:
    wfi
    j 2b
