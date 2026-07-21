#include "gem5_test.h"

enum
{
    WordCount = 64
};

static volatile uint32_t ready[2];
static volatile uint32_t words[WordCount];

void
gem5_test_main(void)
{
    const uint32_t hart = hart_id();
    if (hart >= 2) {
        test_fail();
    }

    for (uint32_t index = hart; index < WordCount; index += 2) {
        words[index] = 0x13570000u ^ (index * 0x1021u) ^ hart;
    }
    memory_fence();
    ready[hart] = 1;
    memory_fence();

    while (ready[hart ^ 1u] == 0) {
        compiler_barrier();
    }
    memory_fence();

    for (uint32_t index = 0; index < WordCount; ++index) {
        const uint32_t owner = index & 1u;
        const uint32_t expected =
            0x13570000u ^ (index * 0x1021u) ^ owner;
        if (words[index] != expected) {
            test_fail();
        }
    }
    test_idle();
}
