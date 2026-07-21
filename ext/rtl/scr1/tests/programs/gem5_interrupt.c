#include "gem5_test.h"

extern void trap_entry(void);

volatile uint32_t interrupt_count;
static volatile uint32_t ready[2];

static void
write_csr_mtvec(uint32_t value)
{
    __asm__ volatile("csrw mtvec, %0" : : "r"(value) : "memory");
}

void
gem5_test_main(void)
{
    const uint32_t hart = hart_id();
    if (hart >= 2) {
        test_fail();
    }

    volatile uint32_t *const timer_control =
        (volatile uint32_t *)0x00490000u;
    *timer_control = 0;
    if (hart == 0) {
        write_csr_mtvec((uint32_t)(uintptr_t)&trap_entry);
        __asm__ volatile("csrw mie, %0" : : "r"(8u) : "memory");
        __asm__ volatile("csrsi mstatus, 8" ::: "memory");
    }
    ready[hart] = 1;
    memory_fence();
    while (ready[hart ^ 1u] == 0) {
        compiler_barrier();
    }

    if (hart != 0) {
        test_idle();
    }

    __asm__ volatile("wfi" ::: "memory");
    memory_fence();
    if (interrupt_count != 1) {
        test_fail();
    }
    test_idle();
}
