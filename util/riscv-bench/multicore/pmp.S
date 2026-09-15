# SPDX-License-Identifier: BSD-3-Clause

.section .text.init,"ax"
.globl _start
.option norvc
_start:
    csrr t0, mhartid
    bnez t0, park
    la t0, trap
    csrw mtvec, t0
    la s0, data
#if CASE == 5 || CASE == 6
    srli t0, s0, 2
    ori t0, t0, 511
    csrw pmpaddr0, t0
    li t0, 0x1f
    csrw pmpcfg0, t0
#if CASE == 6
    jalr ra, s0, 0
    li t0, 0x99  # Lock and revoke execute permission after warming fetch.
    csrw pmpcfg0, t0
    li s1, 1
    jalr ra, s0, 0
    j fail
#else
    li t0, 0x20800  # MPRV + MPP=S
    csrs mstatus, t0
    ld t0, 0(s0)
    li t0, 0x18  # Revoke data permission after warming translation.
    csrw pmpcfg0, t0
    li s1, 5
    ld t0, 0(s0)
    j fail
#endif
#endif
    # Entry 0: four read-only bytes in the middle of an eight-byte load.
    addi t0, s0, 4
    srli t0, t0, 2
    csrw pmpaddr0, t0
    li t0, -1
    csrw pmpaddr1, t0
    li t0, 0x1f11 # NA4+R; lower priority NAPOT RWX
    csrw pmpcfg0, t0
    li s1, 5
#if CASE != 4
    li t0, 0x21800 # MPRV + MPP=M initially
    csrc mstatus, t0
    li t0, 0x20800 # MPRV + MPP=S
    csrs mstatus, t0
#endif
    ld t0, 0(s0)
    j fail
trap:
    csrr t0, mcause
    bne t0, s1, fail
    csrr t0, mtval
    bne t0, s0, fail
    li a0, 0
    .word 0x4200007b
park: j park
fail:
    li a0, 0
    li a1, 71
    .word 0x4400007b
    j fail
.section .data
.balign 4096
data: .word 0x01100513, 0x00008067
.space 4096
