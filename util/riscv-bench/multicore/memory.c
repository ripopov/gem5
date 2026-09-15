/* SPDX-License-Identifier: BSD-3-Clause */

typedef unsigned long u64;
struct slot
{
    volatile u64 value;
    u64 pad[7];
};
static struct slot arrived[NHARTS] __attribute__((aligned(256)));
static struct slot ready, done;
static struct slot cells[4] __attribute__((aligned(256)));
static volatile u64 payload[64] __attribute__((aligned(256)));

static void
fence(void)
{
    __asm__ volatile("fence rw, rw" ::: "memory");
}
__attribute__((noreturn)) static void
fail(u64 code)
{
    register u64 a0 __asm__("a0") = 0;
    register u64 a1 __asm__("a1") = code;
    __asm__ volatile(".word 0x4400007b" ::"r"(a0), "r"(a1) : "memory");
    for (;;) {}
}
static void
wait(volatile u64 *p, u64 value)
{
    u64 timeout = 100000;
    while (*p != value) {
        if (!--timeout) {
            fail(90);
        }
    }
    fence();
}
static void
barrier(u64 hart)
{
    fence();
    u64 phase = ++arrived[hart].value;
    for (u64 i = 0; i < NHARTS; i++) {
        u64 timeout = 100000;
        while (arrived[i].value < phase) {
            if (!--timeout) {
                fail(91);
            }
        }
    }
    fence();
}
static u64
lr(volatile u64 *p)
{
    u64 v;
    __asm__ volatile("lr.d.aq %0, (%1)" : "=r"(v) : "r"(p) : "memory");
    return v;
}
static u64
sc(volatile u64 *p, u64 v)
{
    u64 status;
    __asm__ volatile("sc.d.rl %0, %2, (%1)"
                     : "=r"(status)
                     : "r"(p), "r"(v)
                     : "memory");
    return status;
}
static u64
add(volatile u64 *p, u64 v)
{
    u64 old;
    __asm__ volatile("amoadd.d.aqrl %0, %2, (%1)"
                     : "=r"(old)
                     : "r"(p), "r"(v)
                     : "memory");
    return old;
}
static void
run_test(u64 hart)
{
    if (!hart) {
        ready.value = done.value = 0;
        for (u64 i = 0; i < 4; i++) {
            cells[i].value = 0;
        }
    }
    barrier(hart);
#if CASE <= 4 || CASE == 8 || CASE == 9
    for (u64 channel = 0; channel < 4; channel++) {
        volatile u64 *p = &cells[channel].value;
#if CASE == 3 || CASE == 4 || CASE == 8
        /* Reserve a later 16-byte granule in the same cache line. */
        p += CASE == 8 ? 2 : 3;
#endif
        if (!hart) {
            *p = 17;
        }
        barrier(hart);
        if (!hart) {
            if (lr(p) != 17) {
                fail(1);
            }
            fence();
            ready.value = channel + 1;
            wait(&done.value, channel + 1);
            if (!sc(p, 99)) {
                fail(10 + channel);
            }
        } else if (hart == 1) {
            wait(&ready.value, channel + 1);
#if CASE == 1
            *p = 19;
#elif CASE == 8
            /* Start in the previous granule, but overwrite reserved bytes. */
            volatile unsigned char *start = (volatile unsigned char *)p - 3;
            u64 value = 0x1122334455667788UL;
            __asm__ volatile("sd %1, 0(%0)" ::"r"(start), "r"(value)
                             : "memory");
#elif CASE == 9
            *p = 19;
            (void)lr(p);
#elif CASE == 2
            add(p, 2);
#elif CASE == 3
            volatile u64 *base = &cells[channel].value;
            __asm__ volatile("vsetivli zero, 4, e64, m1, ta, ma\n"
                             "vmv.v.i v0, 0\n"
                             "vse64.v v0, (%0)" ::"r"(base)
                             : "memory");
#elif CASE == 4
            volatile u64 *base = &cells[channel].value;
            __asm__ volatile("cbo.zero (%0)" ::"r"(base) : "memory");
#endif
            fence();
            done.value = channel + 1;
        }
        barrier(hart);
    }
#elif CASE == 5 || CASE == 6
    for (u64 j = 0; j < 1000; j++) {
        volatile u64 *p = &cells[j % 4].value;
#if CASE == 6
        if (hart & 1) {
            add(p, 1);
            continue;
        }
#endif
        u64 status;
        __asm__ volatile("1: lr.d.aq t0, (%1)\n"
                         "addi t0, t0, 1\n"
                         "sc.d.rl %0, t0, (%1)\n"
                         "bnez %0, 1b"
                         : "=&r"(status)
                         : "r"(p)
                         : "t0", "memory");
    }
    barrier(hart);
    if (!hart) {
        for (u64 i = 0; i < 4; i++) {
            if (cells[i].value != 250 * NHARTS) {
                fail(20 + i);
            }
        }
    }
#elif CASE == 7
    /* Ring: every hart verifies then replaces the previous producer's data. */
    for (u64 round = 0; round < 100; round++) {
        u64 sequence = round * NHARTS + hart;
        wait(&ready.value, sequence);
        if (sequence) {
            for (u64 j = 0; j < 64; j++) {
                if (payload[j] != (sequence - 1) * 64 + j) {
                    fail(30);
                }
            }
        }
        for (u64 j = 0; j < 64; j++) {
            payload[j] = sequence * 64 + j;
        }
        fence();
        ready.value = sequence + 1;
    }
#elif CASE == 10
    for (u64 channel = 0; channel < 4; channel++) {
        volatile u64 *p = &cells[channel].value;
        (void)lr(p);
        barrier(hart);
        u64 status = sc(p, hart + 1);
        /* Every hart reserved the original value: exactly one may win. */
        if (!status) {
            add(&done.value, 1);
        }
        barrier(hart);
        if (!hart && done.value != channel + 1) {
            fail(40 + channel);
        }
    }
#elif CASE == 11
    volatile u64 *rom = (volatile u64 *)0x90000000;
    if (add(rom, 1) != 17 || *rom != 17) {
        fail(50);
    }
#elif CASE == 12
    /* Publish new instruction bytes, then consume them after FENCE.I. */
    for (u64 round = 0; round < 100; round++) {
        u64 sequence = round * NHARTS + hart;
        wait(&ready.value, sequence);
        __asm__ volatile("fence.i" ::: "memory");
        if (sequence) {
            u64 (*code)(void) = (u64 (*)(void))payload;
            if (code() != sequence) {
                fail(60);
            }
        }
        payload[0] = (0x00008067UL << 32) | ((sequence + 1) << 20) | 0x513;
        fence();
        ready.value = sequence + 1;
    }
#endif
    barrier(hart);
}

void
test_main(u64 hart)
{
    for (u64 epoch = 0; epoch <= SWITCHES; epoch++) {
        run_test(hart);
        if (epoch != SWITCHES) {
            if (!hart) {
                register u64 a0 __asm__("a0") = 0;
                register u64 a1 __asm__("a1") = 0;
                __asm__ volatile(".word 0xb400007b" ::"r"(a0), "r"(a1)
                                 : "memory");
            }
            barrier(hart);
        }
    }
    if (!hart) {
        register u64 a0 __asm__("a0") = 0;
        __asm__ volatile(".word 0x4200007b" ::"r"(a0) : "memory");
    }
    for (;;) {}
}
