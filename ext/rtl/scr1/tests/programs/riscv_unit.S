/* A deterministic RV32I arithmetic/branch smoke test. Success reaches WFI;
 * failure spins until rtl-cosim-check's cycle limit. */

    .section .boot, "ax"
    .global _start
_start:
    li      t0, 17
    li      t1, 25
    add     t2, t0, t1
    li      t3, 42
    bne     t2, t3, fail

    li      t0, 0x00490000
    sw      zero, 0(t0)
    wfi

fail:
    j       fail
