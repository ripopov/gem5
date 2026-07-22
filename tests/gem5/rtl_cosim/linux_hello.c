/*
 * Copyright (c) 2026 The gem5 Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Minimal Linux userspace payload for the JitCPU-to-C910 demonstration.
 * It reaches the m5 exit pseudo instruction on JitCPU. gem5 resumes at the
 * following instruction on C910, so all observable work below the boundary
 * executes on RTL. ASCII EOT asks the modeled 8250 UART to stop simulation.
 */

typedef unsigned long size_t;

static long
linux_syscall3(unsigned long number, unsigned long argument0,
               unsigned long argument1, unsigned long argument2)
{
    register unsigned long a0 __asm__("a0") = argument0;
    register unsigned long a1 __asm__("a1") = argument1;
    register unsigned long a2 __asm__("a2") = argument2;
    register unsigned long a7 __asm__("a7") = number;

    __asm__ volatile(
        "ecall"
        : "+r" (a0)
        : "r" (a1), "r" (a2), "r" (a7)
        : "memory");
    return (long)a0;
}

static long
linux_write(unsigned long fd, const void *buffer, size_t size)
{
    return linux_syscall3(
        64, fd, (unsigned long)buffer, (unsigned long)size);
}

static void
prepare_console(void)
{
    static const char console[] = "/dev/ttyS0";
    const long fd = linux_syscall3(
        56, (unsigned long)-100, (unsigned long)console, 1);

    if (fd < 0) {
        return;
    }
    if (fd != 1) {
        linux_syscall3(24, (unsigned long)fd, 1, 0);
    }
    if (fd != 2) {
        linux_syscall3(24, (unsigned long)fd, 2, 0);
    }
}

static int
write_all(const char *buffer, size_t size)
{
    while (size != 0) {
        long written = linux_write(1, buffer, size);
        if (written <= 0) {
            return -1;
        }
        buffer += written;
        size -= (size_t)written;
    }
    return 0;
}

__attribute__((noreturn)) void
_start(void)
{
    static const char hello[] =
        "Hello world from Linux on the C910 RTL CPU!\n";
    static const char failure[] = "C910 RTL hello write failed\n";
    static const char eot = '\004';
    register unsigned long delay __asm__("a0") = 0;

    /* File-descriptor setup is deliberately part of the fast-CPU phase. */
    prepare_console();

    /* M5OP_EXIT (0x21). This instruction executes on JitCPU. */
    __asm__ volatile(".word 0x4200007b" : "+r" (delay) : : "memory");

    if (write_all(hello, sizeof(hello) - 1) != 0) {
        write_all(failure, sizeof(failure) - 1);
    }
    write_all(&eot, 1);

    /* The EOT schedules gem5's exit event; keep PID 1 alive until it fires. */
    for (;;) {
        __asm__ volatile("nop");
    }
}
