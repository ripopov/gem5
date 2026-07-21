#include "gem5_test.h"

static volatile uint32_t boot_count[2];

void
gem5_test_main(void)
{
    const uint32_t hart = hart_id();
    if (hart >= 2) {
        test_fail();
    }

    const uint32_t count = boot_count[hart] + 1u;
    boot_count[hart] = count;
    memory_fence();
    while (boot_count[hart ^ 1u] == 0) {
        compiler_barrier();
    }

    if ((hart == 0 && count > 2) || (hart == 1 && count != 1)) {
        test_fail();
    }
    test_idle();
}
