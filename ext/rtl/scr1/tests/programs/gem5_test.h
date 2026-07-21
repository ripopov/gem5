#ifndef RTL_COSIM_SCR1_GEM5_TEST_H
#define RTL_COSIM_SCR1_GEM5_TEST_H

#include <stdint.h>

static inline uint32_t
hart_id(void)
{
    uint32_t value;
    __asm__ volatile("csrr %0, mhartid" : "=r"(value));
    return value;
}

static inline void
compiler_barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static inline void
memory_fence(void)
{
    __asm__ volatile("fence rw, rw" ::: "memory");
}

__attribute__((noreturn)) static inline void
test_fail(void)
{
    for (;;) {
        compiler_barrier();
    }
}

__attribute__((noreturn)) static inline void
test_idle(void)
{
    volatile uint32_t *const timer_control =
        (volatile uint32_t *)0x00490000u;
    *timer_control = 0;
    memory_fence();
    for (;;) {
        __asm__ volatile("wfi" ::: "memory");
    }
}

#endif
