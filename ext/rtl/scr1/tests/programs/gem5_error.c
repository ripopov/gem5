#include "gem5_test.h"

extern void gem5_error_trap(void);
extern void gem5_trigger_bad_load(const volatile uint32_t *address);

volatile uint32_t error_ready = 0;
volatile uint32_t error_seen = 0;

void
gem5_test_main(void)
{
    if (hart_id() != 0) {
        error_ready = 1;
        memory_fence();
        test_idle();
    }

    while (error_ready == 0) {
        compiler_barrier();
    }
    memory_fence();

    __asm__ volatile("csrw mtvec, %0" : : "r"(gem5_error_trap) : "memory");
    gem5_trigger_bad_load((const volatile uint32_t *)0x01000000u);
    memory_fence();
    if (error_seen != 1) {
        test_fail();
    }
    test_idle();
}
