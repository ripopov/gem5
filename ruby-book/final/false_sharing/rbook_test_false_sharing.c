/*
 * rbook_test_false_sharing.c -- Stage 3c false-sharing benchmark.
 *
 * Main runs on CPU 0. Fifteen pthreads are created so one deterministic worker
 * lands on CPU 15 in gem5 SE mode. Those two participants exchange a
 * ping-pong token across the mesh.
 *
 * The benchmark measures two round-trip latencies:
 * 1. a control ping-pong that uses only separate cache-line control/data words
 * 2. a false-sharing ping-pong where both cores also write adjacent ints in
 *    the same 64-byte cache line
 *
 * The latency delta between those phases is the cleanest estimate of the
 * additional coherence cost caused by ownership bouncing on the false-shared
 * line.
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
    struct AlignedInt private_words[2];
    struct AlignedInt req_flag;
    struct AlignedInt ack_flag;
    int iterations;
    int worker_cpu;
    int participant_index;
    int worker_cpus[NUM_WORKERS];
    pthread_barrier_t ready_barrier;
    pthread_barrier_t pair_barrier;
};

struct WorkerArg
{
    struct SharedState *state;
    int thread_index;
};

struct SampleSummary
{
    uint64_t min;
    uint64_t max;
    double avg;
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
reset_ping_pong_flags(struct SharedState *state)
{
    store_flag(&state->req_flag, 0);
    store_flag(&state->ack_flag, 0);
}

static void
log_phase(const char *phase)
{
    printf("RBOOK_FALSE_PHASE %s\n", phase);
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

static void
wait_barrier(pthread_barrier_t *barrier, const char *name)
{
    int rc = pthread_barrier_wait(barrier);

    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
        fprintf(stderr, "%s failed: %d\n", name, rc);
        exit(1);
    }
}

static struct SampleSummary
summarize_samples(const uint64_t *samples, int count)
{
    struct SampleSummary summary;
    uint64_t total = 0;

    summary.min = samples[0];
    summary.max = samples[0];
    summary.avg = 0.0;

    for (int i = 0; i < count; ++i) {
        if (samples[i] < summary.min) {
            summary.min = samples[i];
        }
        if (samples[i] > summary.max) {
            summary.max = samples[i];
        }
        total += samples[i];
    }

    summary.avg = (double)total / (double)count;
    return summary;
}

static void
run_control_cpu0(struct SharedState *state, uint64_t *samples)
{
    for (int round = 1; round <= state->iterations; ++round) {
        uint64_t start = rdcycle();
        uint64_t end;

        state->private_words[0].value += 1;
        store_flag(&state->req_flag, round);
        wait_for_flag(&state->ack_flag, round);
        end = rdcycle();

        samples[round - 1] = end - start;
    }
}

static void
run_false_cpu0(struct SharedState *state, uint64_t *samples)
{
    for (int round = 1; round <= state->iterations; ++round) {
        uint64_t start = rdcycle();
        uint64_t end;

        state->shared_words[0] += 1;
        store_flag(&state->req_flag, round);
        wait_for_flag(&state->ack_flag, round);
        end = rdcycle();

        samples[round - 1] = end - start;
    }
}

static void
run_control_cpu15(struct SharedState *state)
{
    for (int round = 1; round <= state->iterations; ++round) {
        wait_for_flag(&state->req_flag, round);
        state->private_words[1].value += 1;
        store_flag(&state->ack_flag, round);
    }
}

static void
run_false_cpu15(struct SharedState *state)
{
    for (int round = 1; round <= state->iterations; ++round) {
        wait_for_flag(&state->req_flag, round);
        state->shared_words[1] += 1;
        store_flag(&state->ack_flag, round);
    }
}

static void *
worker_main(void *arg)
{
    struct WorkerArg *worker = arg;
    struct SharedState *state = worker->state;
    int cpu = gem5_cpu_id();

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

    sink += state->private_words[1].value;
    wait_barrier(&state->pair_barrier, "control pair barrier");
    run_control_cpu15(state);
    wait_barrier(&state->pair_barrier, "control done barrier");

    sink += state->shared_words[1];
    wait_barrier(&state->pair_barrier, "false pair barrier");
    run_false_cpu15(state);
    wait_barrier(&state->pair_barrier, "false done barrier");

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
    struct SampleSummary control_summary;
    struct SampleSummary false_summary;
    uint64_t *control_samples = NULL;
    uint64_t *false_samples = NULL;
    int main_cpu;

    memset(&state, 0, sizeof(state));
    state.shared_words = shared_words;
    state.iterations = parse_iterations(argc, argv);
    state.worker_cpu = -1;
    state.participant_index = -1;

    control_samples = calloc(state.iterations, sizeof(*control_samples));
    false_samples = calloc(state.iterations, sizeof(*false_samples));
    if (!control_samples || !false_samples) {
        fprintf(stderr, "sample allocation failed\n");
        return 1;
    }

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

    state.private_words[0].value = 0;
    state.private_words[1].value = 0;
    shared_words[0] = 0;
    shared_words[1] = 0;
    reset_ping_pong_flags(&state);

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

    reset_ping_pong_flags(&state);
    sink += state.private_words[0].value;
    wait_barrier(&state.pair_barrier, "control pair barrier");
    log_phase("CONTROL_WARMUP_DONE");

    log_phase("CONTROL_MEASURED_START");
    m5_reset_stats(0, 0);
    run_control_cpu0(&state, control_samples);
    wait_barrier(&state.pair_barrier, "control done barrier");
    m5_dump_reset_stats(0, 0);
    log_phase("CONTROL_MEASURED_DONE");

    reset_ping_pong_flags(&state);
    sink += state.shared_words[0];
    wait_barrier(&state.pair_barrier, "false pair barrier");
    log_phase("FALSE_WARMUP_DONE");

    log_phase("FALSE_MEASURED_START");
    m5_reset_stats(0, 0);
    run_false_cpu0(&state, false_samples);
    wait_barrier(&state.pair_barrier, "false done barrier");
    m5_dump_reset_stats(0, 0);
    log_phase("FALSE_MEASURED_DONE");

    pthread_join(threads[state.participant_index], NULL);

    if (state.private_words[0].value != state.iterations ||
        state.private_words[1].value != state.iterations) {
        fprintf(stderr,
                "unexpected control values: value0=%d value1=%d expected=%d\n",
                state.private_words[0].value, state.private_words[1].value,
                state.iterations);
        return 1;
    }

    if (state.shared_words[0] != state.iterations ||
        state.shared_words[1] != state.iterations) {
        fprintf(stderr,
                "unexpected shared values: value0=%d value1=%d expected=%d\n",
                state.shared_words[0], state.shared_words[1],
                state.iterations);
        return 1;
    }

    control_summary = summarize_samples(control_samples, state.iterations);
    false_summary = summarize_samples(false_samples, state.iterations);

    printf("RBOOK_FALSE ITERATIONS %d\n", state.iterations);
    printf("RBOOK_FALSE SHARED_OFFSET 0x%x\n", SHARED_OFFSET);
    printf("RBOOK_FALSE CPU0 %d\n", main_cpu);
    printf("RBOOK_FALSE CPU15 %d\n", state.worker_cpu);
    printf("RBOOK_FALSE CONTROL_AVG %.2f\n", control_summary.avg);
    printf("RBOOK_FALSE CONTROL_MIN %" PRIu64 "\n", control_summary.min);
    printf("RBOOK_FALSE CONTROL_MAX %" PRIu64 "\n", control_summary.max);
    printf("RBOOK_FALSE FALSE_AVG %.2f\n", false_summary.avg);
    printf("RBOOK_FALSE FALSE_MIN %" PRIu64 "\n", false_summary.min);
    printf("RBOOK_FALSE FALSE_MAX %" PRIu64 "\n", false_summary.max);
    printf("RBOOK_FALSE DELTA_AVG %.2f\n",
           false_summary.avg - control_summary.avg);
    printf("RBOOK_FALSE CONTROL_VALUE0 %d\n", state.private_words[0].value);
    printf("RBOOK_FALSE CONTROL_VALUE1 %d\n", state.private_words[1].value);
    printf("RBOOK_FALSE VALUE0 %d\n", state.shared_words[0]);
    printf("RBOOK_FALSE VALUE1 %d\n", state.shared_words[1]);
    printf("PASS\n");
    fflush(stdout);

    pthread_barrier_destroy(&state.pair_barrier);
    pthread_barrier_destroy(&state.ready_barrier);
    free(control_samples);
    free(false_samples);
    return 0;
}
