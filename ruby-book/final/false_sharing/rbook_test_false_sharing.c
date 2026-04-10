/*
 * rbook_test_false_sharing.c -- Stage 3c false-sharing benchmark.
 *
 * Main runs on CPU 0. Fifteen pthreads are created so one deterministic worker
 * lands on CPU 15 in gem5 SE mode. Those two participants repeatedly update
 * adjacent ints on the same 64-byte line, forcing ownership to bounce across
 * the mesh diagonal.
 *
 * Setup uses pthread barriers before the measured window because the probe
 * binary shows they behave correctly in RISC-V SE mode. The measured region
 * itself uses only a shared start flag and the false-shared line.
 */

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <gem5/m5ops.h>

#define CACHE_LINE 64
#define PAGE_BYTES 4096
#define SHARED_OFFSET (15 * CACHE_LINE)
#define DEFAULT_ITERATIONS 128
#define PROGRESS_UPDATES 8
#define NUM_WORKERS 15

static volatile uint64_t sink;

struct AlignedInt
{
    int value;
    char pad[CACHE_LINE - sizeof(int)];
} __attribute__((aligned(CACHE_LINE)));

struct SharedState
{
    volatile int *shared_words;
    int iterations;
    int worker_cpu;
    int participant_index;
    uint64_t worker_cycles;
    int worker_cpus[NUM_WORKERS];
    pthread_barrier_t ready_barrier;
    pthread_barrier_t pair_barrier;
    struct AlignedInt start_flag;
    struct AlignedInt participant_done;
};

struct WorkerArg
{
    struct SharedState *state;
    int thread_index;
};

static inline uint64_t
rdcycle(void)
{
    uint64_t c;

    asm volatile("rdcycle %0" : "=r"(c));
    return c;
}

static inline int
gem5_cpu_id(void)
{
    unsigned cpu = 0;
    unsigned node = 0;
    long ret = syscall(SYS_getcpu, &cpu, &node, 0);

    return ret == 0 ? (int)cpu : -1;
}

static int
parse_iterations(int argc, char **argv)
{
    char *end = NULL;
    long value;

    if (argc == 1) {
        return DEFAULT_ITERATIONS;
    }

    if (argc != 2) {
        fprintf(stderr, "usage: %s [iterations]\n", argv[0]);
        exit(1);
    }

    value = strtol(argv[1], &end, 0);
    if (end == argv[1] || *end != '\0' || value <= 0 || value > INT32_MAX) {
        fprintf(stderr, "invalid iteration count: %s\n", argv[1]);
        exit(1);
    }

    return (int)value;
}

static inline int
load_flag(const struct AlignedInt *flag)
{
    return __atomic_load_n(&flag->value, __ATOMIC_ACQUIRE);
}

static inline void
store_flag(struct AlignedInt *flag, int value)
{
    __atomic_store_n(&flag->value, value, __ATOMIC_RELEASE);
}

static void
wait_for_flag(const struct AlignedInt *flag, int expected)
{
    while (load_flag(flag) != expected) {
        asm volatile("" ::: "memory");
    }
}

static void
log_phase(const char *phase)
{
    printf("RBOOK_FALSE_PHASE %s\n", phase);
    fflush(stdout);
}

static void
log_progress(int completed, int total)
{
    printf("RBOOK_FALSE_PROGRESS %d/%d\n", completed, total);
    fflush(stdout);
}

static void *
map_anonymous_buffer(size_t size)
{
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (ptr == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    return ptr;
}

static void
fault_in_pages(volatile uint8_t *buf, size_t size)
{
    for (size_t i = 0; i < size; i += PAGE_BYTES) {
        sink += buf[i];
    }
}

static __attribute__((noinline)) void
run_updates(volatile int *slot, int iterations, int emit_progress)
{
    int next_progress = 0;
    int progress_stride = 0;

    if (emit_progress) {
        progress_stride = iterations / PROGRESS_UPDATES;
        if (progress_stride <= 0) {
            progress_stride = 1;
        }
        next_progress = progress_stride;
    }

    for (int i = 0; i < iterations; ++i) {
        *slot = *slot + 1;

        if (emit_progress && (i + 1 >= next_progress || i + 1 == iterations)) {
            log_progress(i + 1, iterations);
            next_progress += progress_stride;
        }
    }

    asm volatile("" ::: "memory");
}

static void
wait_barrier(pthread_barrier_t *barrier, const char *name)
{
    int rc = pthread_barrier_wait(barrier);

    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
        fprintf(stderr, "%s failed: %d\n", name, rc);
        exit(1);
    }
}

static void *
worker_main(void *arg)
{
    struct WorkerArg *worker = arg;
    struct SharedState *state = worker->state;
    int cpu = gem5_cpu_id();
    uint64_t start;
    uint64_t end;

    if (cpu < 0) {
        fprintf(stderr, "worker getcpu failed\n");
        return NULL;
    }

    state->worker_cpus[worker->thread_index] = cpu;
    if (cpu == 15) {
        state->worker_cpu = cpu;
        state->participant_index = worker->thread_index;
    }

    wait_barrier(&state->ready_barrier, "ready barrier");

    if (cpu != 15) {
        return NULL;
    }

    sink += state->shared_words[1];
    wait_barrier(&state->pair_barrier, "pair barrier");

    wait_for_flag(&state->start_flag, 1);
    start = rdcycle();
    run_updates(&state->shared_words[1], state->iterations, 0);
    end = rdcycle();

    state->worker_cycles = end - start;
    store_flag(&state->participant_done, 1);
    return NULL;
}

int
main(int argc, char **argv)
{
    volatile uint8_t *target_page = map_anonymous_buffer(PAGE_BYTES);
    volatile int *shared_words = (volatile int *)(target_page + SHARED_OFFSET);
    pthread_t threads[NUM_WORKERS];
    struct WorkerArg worker_args[NUM_WORKERS];
    struct SharedState state;
    int main_cpu;
    uint64_t cpu0_cycles_start;
    uint64_t cpu0_cycles_end;
    uint64_t cpu0_cycles;

    memset(&state, 0, sizeof(state));
    state.shared_words = shared_words;
    state.iterations = parse_iterations(argc, argv);
    state.worker_cpu = -1;
    state.participant_index = -1;

    if (pthread_barrier_init(&state.ready_barrier, NULL, NUM_WORKERS + 1) !=
        0) {
        fprintf(stderr, "pthread_barrier_init(ready) failed\n");
        return 1;
    }
    if (pthread_barrier_init(&state.pair_barrier, NULL, 2) != 0) {
        fprintf(stderr, "pthread_barrier_init(pair) failed\n");
        return 1;
    }

    log_phase("START");
    fault_in_pages(target_page, PAGE_BYTES);

    shared_words[0] = 0;
    shared_words[1] = 0;

    main_cpu = gem5_cpu_id();
    if (main_cpu != 0) {
        fprintf(stderr, "expected main on CPU 0, got %d\n", main_cpu);
        return 1;
    }

    for (int i = 0; i < NUM_WORKERS; ++i) {
        int rc;

        worker_args[i].state = &state;
        worker_args[i].thread_index = i;
        rc = pthread_create(&threads[i], NULL, worker_main, &worker_args[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create[%d] failed: %s\n", i,
                    strerror(rc));
            return 1;
        }
    }
    log_phase("THREADS_LAUNCHED");

    wait_barrier(&state.ready_barrier, "ready barrier");
    log_phase("READY_BARRIER_DONE");

    if (state.worker_cpu != 15 || state.participant_index < 0) {
        fprintf(stderr, "failed to discover CPU 15 participant\n");
        return 1;
    }
    log_phase("PARTICIPANT_READY");

    for (int i = 0; i < NUM_WORKERS; ++i) {
        if (i == state.participant_index) {
            continue;
        }
        pthread_join(threads[i], NULL);
    }

    /* Warm the line into both private caches before the measured window. */
    sink += shared_words[0];
    wait_barrier(&state.pair_barrier, "pair barrier");
    log_phase("WARMUP_DONE");

    printf("RBOOK_FALSE READY 1\n");
    fflush(stdout);

    log_phase("MEASURED_START");
    m5_reset_stats(0, 0);

    cpu0_cycles_start = rdcycle();
    store_flag(&state.start_flag, 1);
    run_updates(&shared_words[0], state.iterations, 1);
    cpu0_cycles_end = rdcycle();
    cpu0_cycles = cpu0_cycles_end - cpu0_cycles_start;

    wait_for_flag(&state.participant_done, 1);
    m5_dump_reset_stats(0, 0);
    log_phase("MEASURED_DONE");

    pthread_join(threads[state.participant_index], NULL);

    if (shared_words[0] != state.iterations ||
        shared_words[1] != state.iterations) {
        fprintf(stderr,
                "unexpected final values: value0=%d value1=%d expected=%d\n",
                shared_words[0], shared_words[1], state.iterations);
        return 1;
    }

    printf("RBOOK_FALSE ITERATIONS %d\n", state.iterations);
    printf("RBOOK_FALSE SHARED_OFFSET 0x%x\n", SHARED_OFFSET);
    printf("RBOOK_FALSE CPU0 %d\n", main_cpu);
    printf("RBOOK_FALSE CPU15 %d\n", state.worker_cpu);
    printf("RBOOK_FALSE CPU0_CYCLES %" PRIu64 "\n", cpu0_cycles);
    printf("RBOOK_FALSE CPU15_CYCLES %" PRIu64 "\n", state.worker_cycles);
    printf("RBOOK_FALSE VALUE0 %d\n", shared_words[0]);
    printf("RBOOK_FALSE VALUE1 %d\n", shared_words[1]);
    printf("PASS\n");
    fflush(stdout);

    pthread_barrier_destroy(&state.pair_barrier);
    pthread_barrier_destroy(&state.ready_barrier);
    return 0;
}
