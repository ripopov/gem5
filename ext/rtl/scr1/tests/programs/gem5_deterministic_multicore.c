#include "gem5_test.h"

enum
{
    Rounds = 64
};

static volatile uint32_t turn;
static volatile uint32_t sequence;

void
gem5_test_main(void)
{
    const uint32_t hart = hart_id();
    if (hart >= 2) {
        test_fail();
    }

    for (uint32_t round = 0; round < Rounds; ++round) {
        while (turn != hart) {
            compiler_barrier();
        }
        memory_fence();
        const uint32_t expected = round * 2u + hart;
        if (sequence != expected) {
            test_fail();
        }
        sequence = expected + 1u;
        memory_fence();
        turn = hart ^ 1u;
        memory_fence();
    }

    while (sequence != Rounds * 2u) {
        compiler_barrier();
    }
    test_idle();
}
