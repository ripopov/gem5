# SPDX-License-Identifier: BSD-3-Clause

# Change an Sv39 mapping while preserving the virtual address. Exercise
# either cached fetches or cached data translations around SFENCE.VMA.
.section .text.init,"ax"
.globl _start
.option norvc
_start:
    csrr t0, mhartid
    bnez t0, park
    la t0, trap
    csrw mtvec, t0
    li t0, -1
    csrw pmpaddr0, t0
    li t0, 0x1f
    csrw pmpcfg0, t0

    li s0, 0x40000000
    li s1, 0x80200000
    li s2, 0x80400000
    li t0, 0x0000806701100513 # addi a0,zero,17; ret
    sd t0, 0(s1)
    li t0, 0x0000806701d00513 # addi a0,zero,29; ret
    sd t0, 0(s2)
    fence.i

    la t0, root_table
    li t1, 0x200000cf # identity-map code/stack using a 1GiB leaf
    sd t1, 16(t0)
    la t1, middle_table
    srli t2, t1, 2
    ori t2, t2, 1
    sd t2, 8(t0)
    srli t2, s1, 2
    ori t2, t2, 0xcf
    sd t2, 0(t1)
    srli t0, t0, 12
    li t1, 0x8000000000000000
    or t0, t0, t1
    csrw satp, t0
    sfence.vma
    la t0, supervisor
    csrw mepc, t0
    li t0, 0x1800
    csrc mstatus, t0
    li t0, 0x800
    csrs mstatus, t0
    li s3, 0
    mret
supervisor:
#if CASE == 1
    jalr ra, s0, 0
    li t0, 17
#else
    ld a0, 0(s0)
    ld t0, 0(s1)
#endif
    bne a0, t0, fail
    ecall
#if CASE == 1
    jalr ra, s0, 0
    li t0, 29
#else
    ld a0, 0(s0)
    ld t0, 0(s2)
#endif
    bne a0, t0, fail
    ecall
    j fail
trap:
    csrr t0, mcause
    li t1, 9
    bne t0, t1, fail
    bnez s3, pass
    li s3, 1
    la t0, middle_table
    srli t1, s2, 2
    ori t1, t1, 0xcf
    sd t1, 0(t0)
    sfence.vma s0, zero
    csrr t0, mepc
    addi t0, t0, 4
    csrw mepc, t0
    mret
pass:
    li a0, 0
    .word 0x4200007b
park: j park
fail:
    li a0, 0
    li a1, 73
    .word 0x4400007b
    j fail
.section .bss
.balign 4096
root_table: .space 4096
middle_table: .space 4096
