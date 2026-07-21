#include <stdint.h>

enum
{
    WordCount = 8
};

static const uint32_t seed[WordCount] = {
    0x10203040u, 0x55667788u, 0xdeadbeefu, 0x0badc0deu,
    0x13579bdfu, 0x2468ace0u, 0xa5a55a5au, 0xc001d00du,
};

static volatile uint32_t initialized_cookie = 0x31415926u;
static uint32_t zero_initialized[WordCount];

static uint32_t
mix(uint32_t state, uint32_t value)
{
    return ((state << 5) | (state >> 27)) ^ value;
}

__attribute__((noinline)) static uint32_t
checksum(volatile const uint32_t *words)
{
    uint32_t result = 0;
    for (uint32_t index = 0; index < WordCount; ++index) {
        result = mix(result, words[index]);
    }
    return result;
}

__attribute__((noreturn)) static void
fail(void)
{
    for (;;) {
        __asm__ volatile("" ::: "memory");
    }
}

__attribute__((noreturn)) void
c_integration_test(void)
{
    volatile uint32_t *const timer_control = (volatile uint32_t *)0x00490000u;
    volatile uint32_t *const external_memory =
        (volatile uint32_t *)0x00001000u;
    uint32_t expected = 0;

    *timer_control = 0;

    if (initialized_cookie != 0x31415926u) {
        fail();
    }

    for (uint32_t index = 0; index < WordCount; ++index) {
        if (zero_initialized[index] != 0) {
            fail();
        }

        const uint32_t value = seed[index] ^ (0x01010101u * index);
        external_memory[index] = value;
        expected = mix(expected, value);
    }

    const uint32_t actual = checksum(external_memory);
    if (actual != expected) {
        fail();
    }
    initialized_cookie = actual;
    zero_initialized[0] = actual;

    const uint32_t software_interrupt_enable = 8;
    __asm__ volatile("csrw mie, %0"
                     :
                     : "r"(software_interrupt_enable)
                     : "memory");
    for (;;) {
        __asm__ volatile("wfi" ::: "memory");
    }
}
