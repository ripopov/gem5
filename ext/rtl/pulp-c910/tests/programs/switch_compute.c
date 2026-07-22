#include <stdint.h>

enum
{
    WordCount = 64
};

static volatile uint64_t shared_words[WordCount];

static void
finish(uint64_t value)
{
    *(volatile uint64_t *)UINT64_C(0x01820000) = value;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

void
main(void)
{
    uint64_t checksum = UINT64_C(0xcbf29ce484222325);
    for (uint64_t index = 0; index < WordCount; ++index) {
        const uint64_t value =
            (index + 3) * UINT64_C(0x9e3779b97f4a7c15);
        shared_words[index] = value;
        checksum = (checksum ^ value) * UINT64_C(0x100000001b3);
    }

    const uint64_t live0 = checksum;
    const uint64_t live1 = shared_words[7];
    const uint64_t live2 = shared_words[19];
    const uint64_t live3 = shared_words[37];
    const uint64_t live4 = UINT64_C(0x13579bdf2468ace0);
    const uint64_t live5 = UINT64_C(0xfedcba9876543211);

    // Keep realistic compiler-allocated values live through the switch. The
    // m5 instruction itself has a0/a1 result operands, hence those clobbers.
    __asm__ volatile(
        ".word 0xa400007b"
        :
        : "r"(live0), "r"(live1), "r"(live2), "r"(live3), "r"(live4),
          "r"(live5)
        : "a0", "a1", "memory");

    uint64_t result = live0 ^ live1 ^ live2 ^ live3 ^ live4 ^ live5;
    volatile uint64_t divisor = 97;
    for (uint64_t index = 0; index < WordCount; ++index) {
        const uint64_t old = shared_words[(index * 29) & (WordCount - 1)];
        const uint64_t product = old * (index + 11);
        result ^= product / divisor;
        result += product % divisor;
        shared_words[index] = old ^ result;
    }

    // Recompute the pre-switch state from memory. This catches stale or lost
    // writes independently of the values kept live in integer registers.
    uint64_t expected = UINT64_C(0xcbf29ce484222325);
    for (uint64_t index = 0; index < WordCount; ++index) {
        const uint64_t value =
            (index + 3) * UINT64_C(0x9e3779b97f4a7c15);
        expected = (expected ^ value) * UINT64_C(0x100000001b3);
    }
    if (live0 != expected || live1 !=
            10 * UINT64_C(0x9e3779b97f4a7c15) ||
        live2 != 22 * UINT64_C(0x9e3779b97f4a7c15) ||
        live3 != 40 * UINT64_C(0x9e3779b97f4a7c15) || result == 0) {
        finish(UINT64_C(0xbadc910510000002));
    }
    finish(UINT64_C(0x600dc91051000002));
}
