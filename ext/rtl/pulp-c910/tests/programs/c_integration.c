#include <stdint.h>

static uint64_t initialized = UINT64_C(0x1020304050607080);
static uint64_t zeroed[8];

void
main(void)
{
    uint64_t value = initialized;
    for (uint64_t index = 0; index < 8; ++index) {
        if (zeroed[index] != 0) {
            value = UINT64_C(0xbadc910);
            break;
        }
        value ^= (index + 1) * UINT64_C(0x0101010101010101);
    }
    *(volatile uint64_t *)UINT64_C(0x01800020) = value;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}
