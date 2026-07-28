/* SPDX-License-Identifier: BSD-3-Clause */

    .equ UART,             0x10000000
    .equ MTIMECMP,         0x02004000
    .equ SATP_SV39,        (8 << 60)
    .equ PTE_V,            (1 << 0)
    .equ PTE_R,            (1 << 1)
    .equ PTE_W,            (1 << 2)
    .equ PTE_X,            (1 << 3)
    .equ PTE_U,            (1 << 4)
    .equ PTE_A,            (1 << 6)
    .equ PTE_D,            (1 << 7)
    .equ PTE_LEAF,         (PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D)
    .equ ALIAS_VA,         0x40000000
    .equ LINE_COUNT,       64
    .equ LAST_PHASE,       20

    .section .text.init
    .option norvc
    .globl _start
_start:
    la      sp, stack_top

    /* Allow S-mode to access the complete physical address space. */
    la      t0, machine_trap
    csrw    mtvec, t0
    la      t0, machine_trap_save
    csrw    mscratch, t0
    li      t0, 0x3fffffffffffff
    csrw    pmpaddr0, t0
    li      t0, 0x1f
    csrw    pmpcfg0, t0

    /* CLINT raises MTIP. Enable it while the payload executes in S-mode. */
    li      t0, (1 << 7)
    csrw    mie, t0

    /* Sv39: identity-map 0..1 GiB and 2..3 GiB with superpages. */
    la      t0, root_pt
    li      t1, PTE_LEAF
    sd      t1, 0(t0)
    li      t1, 0x80000000
    srli    t1, t1, 12
    slli    t1, t1, 10
    ori     t1, t1, PTE_LEAF
    sd      t1, 16(t0)

    /* Root[1] -> alias_l1[0] -> alias_l0[0] -> page_a. */
    la      t1, alias_l1
    srli    t1, t1, 12
    slli    t1, t1, 10
    ori     t1, t1, PTE_V
    sd      t1, 8(t0)
    la      t0, alias_l1
    la      t1, alias_l0
    srli    t1, t1, 12
    slli    t1, t1, 10
    ori     t1, t1, PTE_V
    sd      t1, 0(t0)
    la      a0, page_a
    call    set_alias_pte

    la      t0, root_pt
    srli    t0, t0, 12
    li      t1, SATP_SV39
    or      t0, t0, t1
    csrw    satp, t0
    sfence.vma x0, x0

    la      t0, supervisor_start
    csrw    mepc, t0
    csrr    t1, mstatus
    li      t2, ~(3 << 11)
    and     t1, t1, t2
    li      t2, (1 << 11)
    or      t1, t1, t2
    csrw    mstatus, t1
    mret

supervisor_start:
    la      t0, supervisor_trap
    csrw    stvec, t0
    la      t0, trap_save
    csrw    sscratch, t0
    li      t0, (1 << 5)
    csrw    sie, t0
    csrs    sstatus, 2

    la      t0, phase
    sd      zero, 0(t0)
    la      t0, atomic_word
    sd      zero, 0(t0)
    la      t0, page_a
    li      t1, 0x1111222233334444
    sd      t1, 0(t0)
    la      t0, page_b
    li      t1, 0xaaaabbbbccccdddd
    sd      t1, 0(t0)

phase_loop:
    la      t0, phase
    ld      s11, 0(t0)
    mv      a0, s11
    call    print_phase

    andi    t0, s11, 1
    bnez    t0, o3_phase

jit_phase:
    beqz    s11, initial_jit_tests

    /* O3 dirty data must have reached backing memory. */
    li      t1, 0x3000
    add     t1, t1, s11
    la      t2, dirty_lines
    li      t3, LINE_COUNT
    li      t5, 4096
1:
    ld      t4, 0(t2)
    bne     t4, t1, fail_dirty
    add     t2, t2, t5
    addi    t3, t3, -1
    bnez    t3, 1b

    /* O3 changed the PTE while SATP stayed constant. */
    li      t0, ALIAS_VA
    ld      t1, 0(t0)
    li      t2, 0xaaaabbbbccccdddd
    bne     t1, t2, fail_translation

    /* O3 changed code after JIT had translated it. */
    call    patch_target
    la      t0, patch_expected
    lw      t1, 0(t0)
    bne     a0, t1, fail_fence_i

    /* JIT changes the mapping for the following O3 phase. */
    la      a0, page_a
    call    set_alias_pte
    sfence.vma x0, x0
    li      t0, ALIAS_VA
    ld      t1, 0(t0)
    li      t2, 0x1111222233334444
    bne     t1, t2, fail_translation
    addi    a0, s11, 0x40
    call    install_patch_value
    j       common_phase_tests

initial_jit_tests:
    /* Populate QEMU's TLB and TB before O3 changes their backing state. */
    li      t0, ALIAS_VA
    ld      t1, 0(t0)
    li      t2, 0x1111222233334444
    bne     t1, t2, fail_translation
    call    patch_target
    li      t0, 1
    bne     a0, t0, fail_fence_i
    call    timer_test
    li      a0, 0x40
    call    install_patch_value
    j       common_phase_tests

o3_phase:
    /* JIT changed the PTE while the O3 object was switched out. */
    li      t0, ALIAS_VA
    ld      t1, 0(t0)
    li      t2, 0x1111222233334444
    bne     t1, t2, fail_translation

    /* JIT also changed executable code before this takeover. */
    call    patch_target
    la      t0, patch_expected
    lw      t1, 0(t0)
    bne     a0, t1, fail_fence_i

    /* O3 must observe JIT's update, not a stale clean CHI line. */
    li      t1, 0x5000
    addi    t0, s11, -1
    add     t1, t1, t0
    la      t2, clean_lines
    li      t3, LINE_COUNT
    li      t5, 4096
2:
    ld      t4, 0(t2)
    bne     t4, t1, fail_clean
    add     t2, t2, t5
    addi    t3, t3, -1
    bnez    t3, 2b

    /* Leave dirty lines in the hierarchy for reverse takeover. */
    li      t1, 0x3000
    addi    t0, s11, 1
    add     t1, t1, t0
    la      t2, dirty_lines
    li      t3, LINE_COUNT
    li      t5, 4096
3:
    sd      t1, 0(t2)
    add     t2, t2, t5
    addi    t3, t3, -1
    bnez    t3, 3b

    /* Read clean lines again so reverse takeover must invalidate them. */
    la      t2, clean_lines
    li      t3, LINE_COUNT
    li      t5, 4096
4:
    ld      t4, 0(t2)
    add     t2, t2, t5
    addi    t3, t3, -1
    bnez    t3, 4b

    /* Modify a PTE and code, but do not alter SATP. */
    la      a0, page_b
    call    set_alias_pte
    sfence.vma x0, x0
    addi    a0, s11, 1
    call    install_patch_value
    li      t0, 1
    beq     s11, t0, first_o3_timer
    j       common_phase_tests
first_o3_timer:
    call    timer_test

common_phase_tests:
    /* JIT writes behind lines that O3 held clean in the prior phase. */
    andi    t0, s11, 1
    bnez    t0, 5f
    li      t1, 0x5000
    add     t1, t1, s11
    la      t2, clean_lines
    li      t3, LINE_COUNT
    li      t5, 4096
6:
    sd      t1, 0(t2)
    add     t2, t2, t5
    addi    t3, t3, -1
    bnez    t3, 6b
5:
    call    atomic_test
    call    prepare_switch_state

    /* M5OP_SWITCH_CPU (0x52). */
    .word   0xa400007b

    call    verify_switch_state
    la      t0, phase
    ld      t1, 0(t0)
    bne     t1, s11, fail_progress
    addi    t1, t1, 1
    sd      t1, 0(t0)
    li      t2, LAST_PHASE
    bleu    t1, t2, phase_loop

    /* The final interval deliberately runs on O3. */
    mv      a0, t1
    call    print_phase
    li      a0, 0
    .word   0x4200007b       /* M5OP_EXIT (0x21). */
7:
    j       7b

prepare_switch_state:
    li      s0, 0x101
    li      s1, 0x202
    li      s2, 0x303
    li      s3, 0x404
    li      s4, 0x505
    li      s5, 0x606
    li      s6, 0x707
    li      s7, 0x808
    li      s8, 0x909
    li      s9, 0xa0a
    li      s10, 0xb0b
    li      t0, 0x123456789abcdef0
    fmv.d.x f8, t0
    li      t0, 0x0fedcba987654321
    fmv.d.x f9, t0
    ret

verify_switch_state:
    li      t0, 0x101
    bne     s0, t0, fail_register
    li      t0, 0x202
    bne     s1, t0, fail_register
    li      t0, 0x303
    bne     s2, t0, fail_register
    li      t0, 0x404
    bne     s3, t0, fail_register
    li      t0, 0x505
    bne     s4, t0, fail_register
    li      t0, 0x606
    bne     s5, t0, fail_register
    li      t0, 0x707
    bne     s6, t0, fail_register
    li      t0, 0x808
    bne     s7, t0, fail_register
    li      t0, 0x909
    bne     s8, t0, fail_register
    li      t0, 0xa0a
    bne     s9, t0, fail_register
    li      t0, 0xb0b
    bne     s10, t0, fail_register
    fmv.x.d t1, f8
    li      t0, 0x123456789abcdef0
    bne     t1, t0, fail_float
    fmv.x.d t1, f9
    li      t0, 0x0fedcba987654321
    bne     t1, t0, fail_float
    csrr    t0, sscratch
    la      t1, trap_save
    bne     t0, t1, fail_csr
    csrr    t0, satp
    beqz    t0, fail_csr
    ret

atomic_test:
    la      t0, atomic_word
    li      t1, 1
    slli    t4, s11, 1
    amoadd.d t2, t1, (t0)
    bne     t2, t4, fail_atomic
8:
    lr.d    t2, (t0)
    addi    t2, t2, 1
    sc.d    t3, t2, (t0)
    bnez    t3, 8b
    ld      t2, 0(t0)
    addi    t4, t4, 2
    bne     t2, t4, fail_atomic
    ret

timer_test:
    la      t0, timer_irqs
    ld      t1, 0(t0)
    rdtime  t2
    addi    t2, t2, 100
    li      t3, MTIMECMP
    sd      t2, 0(t3)
9:
    ld      t2, 0(t0)
    beq     t1, t2, 9b
    ret

set_alias_pte:
    srli    a0, a0, 12
    slli    a0, a0, 10
    li      t0, PTE_LEAF
    or      a0, a0, t0
    la      t0, alias_l0
    sd      a0, 0(t0)
    ret

install_patch_value:
    /* addi a0, zero, imm */
    la      t0, patch_expected
    sw      a0, 0(t0)
    slli    t1, a0, 20
    li      t2, 0x513
    or      t1, t1, t2
    la      t0, patch_target
    sw      t1, 0(t0)
    fence.i
    ret

    .align  2
patch_target:
    addi    a0, zero, 1
    ret

print_phase:
    li      t0, UART
    li      t1, 'P'
    sb      t1, 0(t0)
    li      t1, 10
    divu    t2, a0, t1
    remu    t3, a0, t1
    addi    t2, t2, '0'
    addi    t3, t3, '0'
    sb      t2, 0(t0)
    sb      t3, 0(t0)
    li      t1, ' '
    sb      t1, 0(t0)
    andi    t1, a0, 1
    beqz    t1, 10f
    li      t1, 'O'
    j       11f
10:
    li      t1, 'J'
11:
    sb      t1, 0(t0)
    li      t1, '\n'
    sb      t1, 0(t0)
    ret

supervisor_trap:
    csrrw   t0, sscratch, t0
    sd      t1, 0(t0)
    sd      t2, 8(t0)
    csrr    t1, scause
    li      t2, 0x8000000000000005
    bne     t1, t2, trap_fail
    la      t1, timer_irqs
    ld      t2, 0(t1)
    addi    t2, t2, 1
    sd      t2, 0(t1)
    li      t1, MTIMECMP
    li      t2, -1
    sd      t2, 0(t1)
    ld      t2, 8(t0)
    ld      t1, 0(t0)
    csrrw   t0, sscratch, t0
    sret

trap_fail:
    csrrw   t0, sscratch, t0
    j       fail_trap

machine_trap:
    csrrw   t0, mscratch, t0
    sd      t1, 0(t0)
    sd      t2, 8(t0)
    csrr    t1, mcause
    li      t2, 0x8000000000000007
    bne     t1, t2, machine_trap_fail
    la      t1, timer_irqs
    ld      t2, 0(t1)
    addi    t2, t2, 1
    sd      t2, 0(t1)
    li      t1, MTIMECMP
    li      t2, -1
    sd      t2, 0(t1)
    ld      t2, 8(t0)
    ld      t1, 0(t0)
    csrrw   t0, mscratch, t0
    mret

machine_trap_fail:
    li      a0, 10
    j       fail

fail_register:    li a0, 1;  j fail
fail_float:       li a0, 2;  j fail
fail_csr:         li a0, 3;  j fail
fail_dirty:       li a0, 4;  j fail
fail_clean:       li a0, 5;  j fail
fail_translation: li a0, 6;  j fail
fail_fence_i:     li a0, 7;  j fail
fail_trap:        li a0, 8;  j fail
fail_progress:    li a0, 9;  j fail
fail_atomic:      li a0, 11; j fail
fail:
    mv      a1, a0
    li      a0, 0
    .word   0x4400007b       /* M5OP_FAIL (0x22). */
12:
    j       12b

    .section .data
    .align 3
phase:          .dword 0
atomic_word:    .dword 0
timer_irqs:     .dword 0
patch_expected: .word 1

    .section .bss
    .align 12
root_pt:        .skip 4096
alias_l1:       .skip 4096
alias_l0:       .skip 4096
page_a:         .skip 4096
page_b:         .skip 4096
trap_save:      .skip 64
machine_trap_save: .skip 64
    .align 12
dirty_lines:    .skip (4096 * LINE_COUNT)
clean_lines:    .skip (4096 * LINE_COUNT)
    .align 4
stack:          .skip 16384
stack_top:
