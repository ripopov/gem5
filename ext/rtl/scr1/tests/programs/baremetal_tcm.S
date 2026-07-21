/* Boot over AXI, execute from the unified TCM, verify AXI data memory, enable
 * software-interrupt wake-up, disable the free-running timer, and enter WFI. */

    .section .boot, "ax"
    .global _start
_start:
    lui     t0, %hi(tcm_entry)
    jalr    zero, %lo(tcm_entry)(t0)

    .section .text, "ax"
tcm_entry:
    li      t0, 0x00490000
    sw      zero, 0(t0)

    li      t0, 0x00001000
    li      t1, 0x5a17c0de
    sw      t1, 0(t0)
    lw      t2, 0(t0)
    bne     t1, t2, fail

    li      t0, 8
    csrw    mie, t0
    wfi

fail:
    j       fail
