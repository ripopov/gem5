/* SPDX-License-Identifier: BSD-3-Clause */

#define _GNU_SOURCE

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef JITCPU_DINING_SWITCHES
#define JITCPU_DINING_SWITCHES 1
#endif

#ifndef JITCPU_DINING_OTHER_PHASE_MEALS
#define JITCPU_DINING_OTHER_PHASE_MEALS 1024
#endif

enum
{
    NUM_PHILOSOPHERS = 16,
    NUM_SHARED_SLOTS = 64,
    NUM_SWITCHES = JITCPU_DINING_SWITCHES,
    FIRST_PHASE_MEALS = 128,
    OTHER_PHASE_MEALS = JITCPU_DINING_OTHER_PHASE_MEALS,
    WAIT_TIMEOUT_SECONDS = 120,
};

_Static_assert(NUM_SWITCHES >= 1, "at least one CPU switch is required");
_Static_assert(NUM_SWITCHES % 2 == 1, "the final CPU must be O3");

static const uint64_t seed_base = UINT64_C(0x9e3779b97f4a7c15);

static pthread_mutex_t forks[NUM_PHILOSOPHERS];
static pthread_mutex_t print_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int fork_owner[NUM_PHILOSOPHERS];
static uint64_t fork_uses[NUM_PHILOSOPHERS];
static _Atomic uint64_t shared_slots[NUM_SHARED_SLOTS];
static _Atomic uint64_t total_meals;
static _Atomic uint64_t shared_atomic_counter;
static _Atomic uint64_t philosopher_meals[NUM_PHILOSOPHERS];
static _Atomic uint64_t philosopher_checksums[NUM_PHILOSOPHERS];
static _Atomic int affinity_ready;
static _Atomic int released_phase = -1;
static _Atomic int completed_workers;

/*
 * Stop the simulation, but only once the console has actually transmitted
 * everything printed so far. The host validates this workload through the
 * markers on the guest terminal, and m5_exit takes effect immediately: the
 * bytes the 8250 has not clocked out yet are simply never written, which
 * truncates the last line the test is looking for.
 */
static void
m5_exit(void)
{
    fflush(NULL);
    tcdrain(STDOUT_FILENO);

    register uintptr_t a0 asm("a0") = 0;
    asm volatile(".word 0x4200007b" : "+r"(a0) : : "memory");
}

__attribute__((noreturn)) static void
fail(unsigned code, const char *message)
{
    char buffer[256];
    int length =
        snprintf(buffer, sizeof(buffer),
                 "JITCPU-DINING FAIL code=%u message=%s\n", code, message);
    if (length > 0) {
        size_t bytes = (size_t)length < sizeof(buffer) ? (size_t)length
                                                       : sizeof(buffer) - 1;
        ssize_t written = write(STDOUT_FILENO, buffer, bytes);
        (void)written;
    }

    register uintptr_t a0 asm("a0") = 0;
    register uintptr_t a1 asm("a1") = code;
    asm volatile(".word 0x4400007b" : "+r"(a0), "+r"(a1) : : "memory");
    _exit((int)code);
}

static uint64_t
initial_seed(unsigned philosopher)
{
    return seed_base ^ (UINT64_C(0xd1b54a32d192ed03) * (philosopher + 1));
}

static uint64_t
next_random(uint64_t state)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static uint64_t
update_checksum(uint64_t checksum, uint64_t state, unsigned meal)
{
    checksum ^= state + seed_base + ((uint64_t)meal << 32);
    return (checksum << 9) | (checksum >> (64 - 9));
}

static void
eat_meals(unsigned philosopher, unsigned first_meal, unsigned count,
          uint64_t *random_state, uint64_t *checksum)
{
    const unsigned left = philosopher;
    const unsigned right = (philosopher + 1) % NUM_PHILOSOPHERS;
    const unsigned first = left < right ? left : right;
    const unsigned second = left < right ? right : left;

    for (unsigned offset = 0; offset < count; ++offset) {
        const unsigned meal = first_meal + offset;
        *random_state = next_random(*random_state);
        const unsigned slot = (*random_state >> 32) % NUM_SHARED_SLOTS;
        const uint64_t delta = ((*random_state >> 8) & 0xff) + 1;

        if (pthread_mutex_lock(&forks[first]) != 0 ||
            pthread_mutex_lock(&forks[second]) != 0) {
            fail(70, "pthread_mutex_lock");
        }
        if (atomic_exchange(&fork_owner[first], (int)philosopher + 1) != 0 ||
            atomic_exchange(&fork_owner[second], (int)philosopher + 1) != 0) {
            fail(71, "mutual exclusion");
        }

        ++fork_uses[left];
        ++fork_uses[right];
        atomic_fetch_add(&shared_slots[slot], delta);
        atomic_fetch_add(&total_meals, 1);
        atomic_fetch_add(&shared_atomic_counter, philosopher + 1);
        atomic_fetch_add(&philosopher_meals[philosopher], 1);
        *checksum = update_checksum(*checksum, *random_state, meal);

        if (atomic_exchange(&fork_owner[second], 0) != (int)philosopher + 1 ||
            atomic_exchange(&fork_owner[first], 0) != (int)philosopher + 1) {
            fail(72, "fork ownership corruption");
        }
        if (pthread_mutex_unlock(&forks[second]) != 0 ||
            pthread_mutex_unlock(&forks[first]) != 0) {
            fail(73, "pthread_mutex_unlock");
        }
    }
}

static int
current_cpu(void)
{
    int cpu = sched_getcpu();
    if (cpu < 0) {
        fail(74, "sched_getcpu");
    }
    return cpu;
}

static unsigned
phase_first_meal(unsigned phase)
{
    if (phase == 0) {
        return 0;
    }
    return FIRST_PHASE_MEALS + (phase - 1) * OTHER_PHASE_MEALS;
}

static unsigned
phase_meals(unsigned phase)
{
    return phase == 0 ? FIRST_PHASE_MEALS : OTHER_PHASE_MEALS;
}

static unsigned
completed_meals(unsigned phase)
{
    return phase_first_meal(phase) + phase_meals(phase);
}

static void *
philosopher_main(void *opaque)
{
    const unsigned philosopher = (unsigned)(uintptr_t)opaque;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(philosopher, &set);
    int error = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (error != 0) {
        fail(75, strerror(error));
    }

    for (unsigned attempt = 0; current_cpu() != (int)philosopher; ++attempt) {
        if (attempt == 1000000) {
            fail(76, "affinity did not take effect");
        }
        sched_yield();
    }

    pthread_mutex_lock(&print_lock);
    printf("JITCPU-DINING AFFINITY philosopher=%u cpu=%d\n", philosopher,
           current_cpu());
    fflush(stdout);
    pthread_mutex_unlock(&print_lock);
    atomic_fetch_add(&affinity_ready, 1);

    uint64_t random_state = initial_seed(philosopher);
    uint64_t checksum = 0;
    for (unsigned phase = 0; phase <= NUM_SWITCHES; ++phase) {
        while (atomic_load(&released_phase) < (int)phase) {
            sched_yield();
        }

        eat_meals(philosopher, phase_first_meal(phase), phase_meals(phase),
                  &random_state, &checksum);
        atomic_store(&philosopher_checksums[philosopher], checksum);
        if (current_cpu() != (int)philosopher) {
            fail(77, "post-switch affinity changed");
        }

        pthread_mutex_lock(&print_lock);
        printf("JITCPU-DINING PHASE philosopher=%u cpu=%d phase=%u meals=%u\n",
               philosopher, current_cpu(), phase, completed_meals(phase));
        if (phase == 1) {
            printf("JITCPU-DINING POST philosopher=%u cpu=%d meals=%u\n",
                   philosopher, current_cpu(), completed_meals(phase));
        }
        fflush(stdout);
        pthread_mutex_unlock(&print_lock);
        atomic_fetch_add(&completed_workers, 1);
    }
    return NULL;
}

static void
wait_for_counter(_Atomic int *counter, int expected, unsigned code,
                 const char *description)
{
    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        fail(code, "clock_gettime");
    }

    while (atomic_load(counter) != expected) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            fail(code, "clock_gettime");
        }
        if (now.tv_sec - start.tv_sec >= WAIT_TIMEOUT_SECONDS) {
            fail(code, description);
        }
        sched_yield();
    }
}

static void
validate_shared_state(unsigned meals_per_philosopher, unsigned code)
{
    uint64_t expected_slots[NUM_SHARED_SLOTS] = {0};
    uint64_t expected_total =
        (uint64_t)NUM_PHILOSOPHERS * meals_per_philosopher;
    uint64_t expected_atomic = expected_total * (NUM_PHILOSOPHERS + 1) / 2;

    for (unsigned philosopher = 0; philosopher < NUM_PHILOSOPHERS;
         ++philosopher) {
        uint64_t random_state = initial_seed(philosopher);
        uint64_t checksum = 0;
        for (unsigned meal = 0; meal < meals_per_philosopher; ++meal) {
            random_state = next_random(random_state);
            unsigned slot = (random_state >> 32) % NUM_SHARED_SLOTS;
            uint64_t delta = ((random_state >> 8) & 0xff) + 1;
            expected_slots[slot] += delta;
            checksum = update_checksum(checksum, random_state, meal);
        }
        if (atomic_load(&philosopher_meals[philosopher]) !=
                meals_per_philosopher ||
            atomic_load(&philosopher_checksums[philosopher]) != checksum) {
            fail(code, "per-philosopher progress or checksum");
        }
    }

    if (atomic_load(&total_meals) != expected_total ||
        atomic_load(&shared_atomic_counter) != expected_atomic) {
        fail(code, "total meal or shared atomic count");
    }
    for (unsigned fork = 0; fork < NUM_PHILOSOPHERS; ++fork) {
        if (fork_uses[fork] != 2 * meals_per_philosopher ||
            atomic_load(&fork_owner[fork]) != 0) {
            fail(code, "fork usage or ownership");
        }
    }
    for (unsigned slot = 0; slot < NUM_SHARED_SLOTS; ++slot) {
        if (atomic_load(&shared_slots[slot]) != expected_slots[slot]) {
            fail(code, "shared slot integrity");
        }
    }
}

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    printf("JITCPU-DINING ONLINE cpus=%ld\n", online);
    if (online != NUM_PHILOSOPHERS) {
        fail(80, "not all 16 Linux CPUs are online");
    }

    pthread_t threads[NUM_PHILOSOPHERS];
    for (unsigned fork = 0; fork < NUM_PHILOSOPHERS; ++fork) {
        if (pthread_mutex_init(&forks[fork], NULL) != 0) {
            fail(81, "pthread_mutex_init");
        }
    }
    for (unsigned philosopher = 0; philosopher < NUM_PHILOSOPHERS;
         ++philosopher) {
        int error =
            pthread_create(&threads[philosopher], NULL, philosopher_main,
                           (void *)(uintptr_t)philosopher);
        if (error != 0) {
            fail(82, strerror(error));
        }
    }

    wait_for_counter(&affinity_ready, NUM_PHILOSOPHERS, 83,
                     "affinity readiness timeout");
    printf("JITCPU-DINING READY workers=16 seed=0x%016" PRIx64 "\n",
           seed_base);
    atomic_store(&released_phase, 0);

    for (unsigned phase = 0; phase <= NUM_SWITCHES; ++phase) {
        wait_for_counter(&completed_workers,
                         (int)((phase + 1) * NUM_PHILOSOPHERS), 84,
                         "phase completion timeout");
        const unsigned meals = completed_meals(phase);
        validate_shared_state(meals, 85);
        printf("JITCPU-DINING PHASE-PASS phase=%u model=%s workers=16 "
               "meals=%u atomic=%" PRIu64 " integrity=ok\n",
               phase, phase % 2 == 0 ? "jit" : "o3", NUM_PHILOSOPHERS * meals,
               atomic_load(&shared_atomic_counter));

        if (phase == 0) {
            printf("JITCPU-DINING PRE-SWITCH-PASS workers=16 meals=%u "
                   "atomic=%" PRIu64 "\n",
                   NUM_PHILOSOPHERS * meals,
                   atomic_load(&shared_atomic_counter));
        } else if (phase == 1) {
            printf("JITCPU-DINING POST-SWITCH-PROGRESS workers=16\n");
        }

        if (phase != NUM_SWITCHES) {
            printf("JITCPU-DINING SWITCH-REQUEST index=%u from=%s to=%s\n",
                   phase + 1, phase % 2 == 0 ? "jit" : "o3",
                   phase % 2 == 0 ? "o3" : "jit");
            /*
             * gem5 resumes at the next statement after switching all CPUs.
             * Workers cannot enter the next phase until that has happened.
             */
            m5_exit();
            atomic_store(&released_phase, (int)phase + 1);
        }
    }

    for (unsigned philosopher = 0; philosopher < NUM_PHILOSOPHERS;
         ++philosopher) {
        int error = pthread_join(threads[philosopher], NULL);
        if (error != 0) {
            fail(88, strerror(error));
        }
    }

    const unsigned total_per_philosopher = completed_meals(NUM_SWITCHES);
    validate_shared_state(total_per_philosopher, 89);
    printf("JITCPU-DINING PASS workers=16 switches=%u meals=%u "
           "atomic=%" PRIu64 " integrity=ok\n",
           NUM_SWITCHES, NUM_PHILOSOPHERS * total_per_philosopher,
           atomic_load(&shared_atomic_counter));
    m5_exit();
    return 0;
}
