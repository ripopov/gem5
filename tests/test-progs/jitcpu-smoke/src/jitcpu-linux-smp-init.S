/* SPDX-License-Identifier: BSD-3-Clause */

    .equ NUM_WORKERS,          4
    .equ PREFLIGHT_PASSES,     16
    .equ WORKING_SET_SIZE,     (64 * 1024)
    .equ CHILD_STACK_SIZE,     (16 * 1024)
    .equ CLONE_VM_SIGCHLD,     0x111
    .equ SYS_SCHED_SETAFFINITY, 122
    .equ SYS_SCHED_YIELD,      124
    .equ SYS_GETCPU,           168
    .equ SYS_CLONE,            220

    .section .text.init
    .option norvc
    .globl _start
_start:
    /* PID 1 is worker zero. Create three address-space-sharing workers. */
    li      s0, 1
create_worker:
    li      t0, NUM_WORKERS
    bge     s0, t0, parent_worker

    addi    t0, s0, -1
    li      t1, CHILD_STACK_SIZE
    mul     t0, t0, t1
    la      a1, child_stacks
    add     a1, a1, t0
    add     a1, a1, t1
    li      a0, CLONE_VM_SIGCHLD
    li      a2, 0
    li      a3, 0
    li      a4, 0
    li      a7, SYS_CLONE
    ecall
    bltz    a0, fail_clone
    beqz    a0, worker_start
    addi    s0, s0, 1
    j       create_worker

parent_worker:
    li      s0, 0

worker_start:
    /* Pin one always-runnable userspace worker to each Linux CPU. */
    addi    sp, sp, -16
    sd      zero, 0(sp)
    sd      zero, 8(sp)
    li      t0, 1
    sll     t0, t0, s0
    sd      t0, 0(sp)
    li      a0, 0
    li      a1, 8
    mv      a2, sp
    li      a7, SYS_SCHED_SETAFFINITY
    ecall
    bltz    a0, fail_affinity

    /* Confirm the kernel really scheduled this task on its assigned hart. */
confirm_cpu:
    addi    a0, sp, 8
    li      a1, 0
    li      a2, 0
    li      a7, SYS_GETCPU
    ecall
    bltz    a0, fail_getcpu
    lwu     t0, 8(sp)
    beq     t0, s0, cpu_confirmed
    li      a7, SYS_SCHED_YIELD
    ecall
    j       confirm_cpu

cpu_confirmed:
    /*
     * Make readiness a meaningful per-hart execution test.  Each worker
     * must finish enough uninterrupted userspace work to cross multiple JIT
     * batch boundaries, touch its private footprint, and contend on a shared
     * AMO line before PID 1 is allowed to signal gem5.
     */
    la      s1, working_sets
    li      t0, WORKING_SET_SIZE
    mul     t1, s0, t0
    add     s1, s1, t1
    add     s2, s1, t0
    li      s3, 1
    li      s4, PREFLIGHT_PASSES
preflight:
    mv      t0, s1
preflight_sweep:
    ld      t1, 0(t0)
    add     t1, t1, s3
    sd      t1, 0(t0)
    addi    t0, t0, 64
    bne     t0, s2, preflight_sweep

    la      t0, shared_counter
    amoadd.d.aqrl t1, s3, (t0)
    addi    s3, s3, 1
    addi    s4, s4, -1
    bnez    s4, preflight

    fence   rw, rw
    la      t0, ready_workers
    li      t1, 1
    amoadd.d.aqrl zero, t1, (t0)

wait_for_workers:
    ld      t1, 0(t0)
    li      t2, NUM_WORKERS
    bne     t1, t2, wait_for_workers
    fence   rw, rw

    /* Only PID 1 signals the host after all four harts reached userspace. */
    bnez    s0, workload
    li      a0, 0
    .word   0x4200007b       /* M5OP_EXIT (0x21). */

workload:
    /* Private footprint per hart plus a contended cross-hart cache line. */
1:
    mv      t0, s1
2:
    ld      t1, 0(t0)
    add     t1, t1, s3
    sd      t1, 0(t0)
    addi    t0, t0, 64
    bne     t0, s2, 2b

    la      t0, shared_counter
    amoadd.d.aqrl t1, s3, (t0)
    addi    s3, s3, 1
    fence   rw, rw
    j       1b

fail_clone:
    li      a1, 20
    j       fail
fail_affinity:
    li      a1, 21
    j       fail
fail_getcpu:
    li      a1, 22
fail:
    li      a0, 0
    .word   0x4400007b       /* M5OP_FAIL (0x22). */
3:
    j       3b

    .section .data
    .align 6
ready_workers:  .dword 0
shared_counter: .dword 0

    .section .bss
    .align 12
child_stacks:
    .skip (CHILD_STACK_SIZE * (NUM_WORKERS - 1))
    .align 12
working_sets:
    .skip (WORKING_SET_SIZE * NUM_WORKERS)
