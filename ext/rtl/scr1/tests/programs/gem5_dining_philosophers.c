#include "gem5_test.h"

enum
{
    MealsPerHart = 32
};

static volatile uint32_t interested[2];
static volatile uint32_t favored;
static volatile uint32_t in_dining_room;
static volatile uint32_t meals[2];
static volatile uint32_t total_meals;

static void
take_both_forks(uint32_t hart)
{
    const uint32_t other = hart ^ 1u;
    interested[hart] = 1;
    compiler_barrier();
    memory_fence();
    favored = other;
    compiler_barrier();
    memory_fence();
    while (interested[other] != 0 && favored == other) {
        compiler_barrier();
    }
    memory_fence();
}

static void
put_down_forks(uint32_t hart)
{
    memory_fence();
    compiler_barrier();
    interested[hart] = 0;
    memory_fence();
}

void
gem5_test_main(void)
{
    const uint32_t hart = hart_id();
    if (hart >= 2) {
        test_fail();
    }

    for (uint32_t meal = 0; meal < MealsPerHart; ++meal) {
        uint32_t thought = 0x9e3779b9u ^ hart ^ meal;
        for (uint32_t step = 0; step < 8; ++step) {
            thought = (thought << 5) ^ (thought >> 3) ^ step;
        }
        compiler_barrier();

        /* Peterson protects the two conceptual forks and dining room. */
        take_both_forks(hart);
        if (in_dining_room != 0) {
            test_fail();
        }
        in_dining_room = thought | 1u;
        memory_fence();
        meals[hart] = meals[hart] + 1u;
        total_meals = total_meals + 1u;
        memory_fence();
        in_dining_room = 0;
        put_down_forks(hart);
    }

    while (meals[hart ^ 1u] != MealsPerHart) {
        compiler_barrier();
    }
    memory_fence();
    if (total_meals != MealsPerHart * 2u ||
        meals[hart] != MealsPerHart) {
        test_fail();
    }
    test_idle();
}
