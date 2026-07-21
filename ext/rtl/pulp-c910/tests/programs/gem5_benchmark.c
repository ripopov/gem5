#include <stdint.h>

enum { Words = 128, Rounds = 4 };

static volatile uint64_t scratch[Words];

static uint64_t
rotateLeft(uint64_t value, unsigned shift)
{
    return (value << shift) | (value >> (64 - shift));
}

void
main(void)
{
    uint64_t state = UINT64_C(0x243f6a8885a308d3);
    for (uint64_t index = 0; index < Words; ++index) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        state *= UINT64_C(0x2545f4914f6cdd1d);
        scratch[index] = state ^
                         (index * UINT64_C(0x9e3779b97f4a7c15));
    }

    uint64_t checksum = UINT64_C(0xcbf29ce484222325);
    for (uint64_t round = 0; round < Rounds; ++round) {
        for (uint64_t index = 0; index < Words; ++index) {
            const uint64_t slot =
                (index * 37 + round * 17) & (Words - 1);
            const uint64_t value = scratch[slot];
            checksum ^= value + index +
                        round * UINT64_C(0x100000001b3);
            checksum = rotateLeft(checksum, 17) *
                       UINT64_C(0x100000001b3);
            scratch[slot] = value ^ checksum;
        }
    }

    checksum = UINT64_C(0xcbf29ce484222325);
    for (uint64_t index = 0; index < Words; ++index) {
        checksum ^= scratch[index];
        checksum *= UINT64_C(0x100000001b3);
    }

    *(volatile uint64_t *)UINT64_C(0x01800020) = checksum;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}
