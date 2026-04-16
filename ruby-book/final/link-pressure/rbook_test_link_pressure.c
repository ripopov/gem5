/*
 * rbook_test_link_pressure.c -- Stage 3g mesh-link pressure benchmark.
 *
 * Designed to put maximum load on the 4x4 mesh physical links so that
 * switching to --per-vnet-links produces a visible throughput gain.
 *
 * Access pattern:
 *   - One shared read-only buffer of STREAM_PAGES * 4 KiB is allocated by
 *     the main thread and pre-faulted/pre-warmed line-by-line so every
 *     64 B block is resident in the LLC before the measured windows.
 *   - Each active thread streams the full buffer cache-line-sequentially.
 *     Because the 16 HNFs are cache-line interleaved on addr[9:6] the
 *     resulting miss stream hits every HNF uniformly -- this is a
 *     many-to-many load, not the single-HNF hotspot of hotspot/.
 *   - Thread i starts at stripe offset (i * 64 B) and wraps once, so the
 *     16 workers target 16 different first-HNFs at the barrier release
 *     and diverge naturally afterwards.
 *
 * Measured state is the LLC-hit steady state: DAT and REQ both cross the
 * mesh, and on read-shared lines SNP/RSP activity is low. All four CHI
 * vnets share the same physical links in the baseline config, so the
 * wide DAT traffic is expected to head-of-line-block REQ flits on the
 * links -- the precise condition --per-vnet-links is designed to relieve.
 *
 * Sweep: 1, 2, 4, 8, 16 active CPUs (same structure as hotspot/) with
 * one m5_reset_stats/m5_dump_reset_stats window per offered-load point.
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
#define LINES_PER_PAGE 64
#define DEFAULT_STREAM_PAGES 1536 /* 6 MiB, fits below 16 MiB LLC total */
#define MAX_STREAM_PAGES 8192
#define STREAM_LANES 8
#define NUM_WORKERS 15
#define NUM_CPUS 16
#define NUM_WINDOWS 5

static const int active_thread_counts[NUM_WINDOWS] = {1, 2, 4, 8, 16};
static volatile uint64_t sink;

struct SharedState
{
    pthread_barrier_t sync_barrier;
    int stream_pages;
    volatile uint64_t *buffer;
    size_t total_lines;
    int worker_cpus[NUM_WORKERS];
    int worker_seen[NUM_WORKERS];
    volatile int active_threads;
    volatile int go_window;
    volatile int done_count;
};

struct WorkerArg
{
    struct SharedState *state;
    int worker_index;
};

static void
fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    fflush(stderr);
    exit(1);
}

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
parse_stream_pages(int argc, char **argv)
{
    char *end = NULL;
    long value;

    if (argc == 1) {
        return DEFAULT_STREAM_PAGES;
    }

    if (argc != 2) {
        fprintf(stderr, "usage: %s [stream-pages]\n", argv[0]);
        exit(1);
    }

    value = strtol(argv[1], &end, 0);
    if (end == argv[1] || *end != '\0' || value <= 0 ||
        value > MAX_STREAM_PAGES) {
        fprintf(stderr, "invalid stream page count: %s\n", argv[1]);
        exit(1);
    }

    return (int)value;
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
warmup_all_lines(volatile uint64_t *buf, size_t total_lines)
{
    uint64_t acc = 0;

    for (size_t line = 0; line < total_lines; line++) {
        acc += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                      line * CACHE_LINE);
    }
    sink += acc;
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

static inline void
wait_for_window(const struct SharedState *state, int target)
{
    while (__atomic_load_n(&state->go_window, __ATOMIC_ACQUIRE) < target) {
        asm volatile("" ::: "memory");
    }
}

static inline void
wait_for_done(const struct SharedState *state, int expected)
{
    while (__atomic_load_n(&state->done_count, __ATOMIC_ACQUIRE) < expected) {
        asm volatile("" ::: "memory");
    }
}

/*
 * Stream read the shared buffer line-by-line starting at start_line and
 * wrapping back to line 0. 8-way unrolling lets the O3 scheduler keep
 * multiple L1D/L2 misses in flight so the sequencer's max-outstanding
 * cap (not the loop latency) is what throttles the miss stream.
 */
static __attribute__((noinline)) void
run_pressure_stream(const volatile uint64_t *buf, size_t total_lines,
                    size_t start_line)
{
    const volatile uint8_t *base = (const volatile uint8_t *)buf;
    uint64_t acc0 = 0, acc1 = 0, acc2 = 0, acc3 = 0;
    uint64_t acc4 = 0, acc5 = 0, acc6 = 0, acc7 = 0;
    size_t i;

#define LOAD_LINE(acc, idx)                                                   \
    acc += *(volatile uint64_t *)(base + (idx) * CACHE_LINE)

    for (i = start_line; i + STREAM_LANES <= total_lines; i += STREAM_LANES) {
        LOAD_LINE(acc0, i + 0);
        LOAD_LINE(acc1, i + 1);
        LOAD_LINE(acc2, i + 2);
        LOAD_LINE(acc3, i + 3);
        LOAD_LINE(acc4, i + 4);
        LOAD_LINE(acc5, i + 5);
        LOAD_LINE(acc6, i + 6);
        LOAD_LINE(acc7, i + 7);
    }
    for (; i < total_lines; i++) {
        LOAD_LINE(acc0, i);
    }

    for (i = 0; i + STREAM_LANES <= start_line; i += STREAM_LANES) {
        LOAD_LINE(acc0, i + 0);
        LOAD_LINE(acc1, i + 1);
        LOAD_LINE(acc2, i + 2);
        LOAD_LINE(acc3, i + 3);
        LOAD_LINE(acc4, i + 4);
        LOAD_LINE(acc5, i + 5);
        LOAD_LINE(acc6, i + 6);
        LOAD_LINE(acc7, i + 7);
    }
    for (; i < start_line; i++) {
        LOAD_LINE(acc0, i);
    }

#undef LOAD_LINE

    sink += acc0 + acc1 + acc2 + acc3 + acc4 + acc5 + acc6 + acc7;
    asm volatile("" ::: "memory");
}

static size_t
start_line_for_cpu(int cpu, size_t total_lines)
{
    return ((size_t)cpu * (total_lines / NUM_CPUS)) % total_lines;
}

static void *
worker_main(void *arg)
{
    struct WorkerArg *worker = arg;
    struct SharedState *state = worker->state;
    int cpu = gem5_cpu_id();

    if (cpu < 0) {
        fail("worker getcpu failed");
    }
    if (cpu <= 0 || cpu >= NUM_CPUS) {
        fprintf(stderr, "FAIL: unexpected worker CPU id %d\n", cpu);
        exit(1);
    }

    state->worker_cpus[worker->worker_index] = cpu;
    __atomic_store_n(&state->worker_seen[worker->worker_index], 1,
                     __ATOMIC_RELEASE);

    wait_barrier(&state->sync_barrier, "setup barrier");

    for (int window = 0; window < NUM_WINDOWS; ++window) {
        wait_barrier(&state->sync_barrier, "window barrier");
        wait_for_window(state, window + 1);

        if (cpu < __atomic_load_n(&state->active_threads, __ATOMIC_ACQUIRE)) {
            run_pressure_stream(state->buffer, state->total_lines,
                                start_line_for_cpu(cpu, state->total_lines));
            __atomic_fetch_add(&state->done_count, 1, __ATOMIC_ACQ_REL);
        }
    }

    return NULL;
}

int
main(int argc, char **argv)
{
    pthread_t threads[NUM_WORKERS];
    struct WorkerArg worker_args[NUM_WORKERS];
    struct SharedState state;
    size_t buffer_bytes;
    volatile uint64_t *shared_buffer;
    int main_cpu;

    memset(&state, 0, sizeof(state));
    state.stream_pages = parse_stream_pages(argc, argv);

    main_cpu = gem5_cpu_id();
    if (main_cpu != 0) {
        fprintf(stderr, "FAIL: expected main on CPU 0, got %d\n", main_cpu);
        return 1;
    }

    if (pthread_barrier_init(&state.sync_barrier, NULL, NUM_CPUS) != 0) {
        fail("pthread_barrier_init(sync) failed");
    }

    buffer_bytes = (size_t)state.stream_pages * PAGE_BYTES;
    shared_buffer = map_anonymous_buffer(buffer_bytes);
    state.buffer = shared_buffer;
    state.total_lines = buffer_bytes / CACHE_LINE;

    /*
     * Warm every line from CPU 0 so subsequent measured windows hit the
     * LLC (mesh-link-bound) rather than the SNF DRAM controller.
     */
    warmup_all_lines(shared_buffer, state.total_lines);

    for (int i = 0; i < NUM_WORKERS; ++i) {
        int rc;

        worker_args[i].state = &state;
        worker_args[i].worker_index = i;
        rc = pthread_create(&threads[i], NULL, worker_main, &worker_args[i]);
        if (rc != 0) {
            fprintf(stderr, "FAIL: pthread_create[%d]: %s\n", i, strerror(rc));
            return 1;
        }
    }

    wait_barrier(&state.sync_barrier, "setup barrier");

    for (int i = 0; i < NUM_WORKERS; ++i) {
        if (!__atomic_load_n(&state.worker_seen[i], __ATOMIC_ACQUIRE)) {
            fprintf(stderr, "FAIL: worker %d never reported CPU\n", i + 1);
            return 1;
        }
        if (state.worker_cpus[i] != i + 1) {
            fprintf(stderr, "FAIL: worker %d expected CPU %d, got %d\n", i + 1,
                    i + 1, state.worker_cpus[i]);
            return 1;
        }
    }

    printf("RBOOK_LINK_PRESSURE MAIN_CPU %d\n", main_cpu);
    printf("RBOOK_LINK_PRESSURE STREAM_PAGES %d\n", state.stream_pages);
    printf("RBOOK_LINK_PRESSURE TOTAL_LINES %zu\n", state.total_lines);
    printf("RBOOK_LINK_PRESSURE BUFFER_BYTES %zu\n", buffer_bytes);
    for (int i = 0; i < NUM_WORKERS; ++i) {
        printf("RBOOK_LINK_PRESSURE WORKER_%02d_CPU %d\n", i + 1,
               state.worker_cpus[i]);
    }
    fflush(stdout);

    for (int window = 0; window < NUM_WINDOWS; ++window) {
        int active_threads = active_thread_counts[window];
        uint64_t start;
        uint64_t end;
        uint64_t total_ops;
        double throughput;

        __atomic_store_n(&state.active_threads, active_threads,
                         __ATOMIC_RELEASE);
        wait_barrier(&state.sync_barrier, "window barrier");

        __atomic_store_n(&state.done_count, 0, __ATOMIC_RELEASE);
        m5_reset_stats(0, 0);
        start = rdcycle();
        __atomic_store_n(&state.go_window, window + 1, __ATOMIC_RELEASE);

        run_pressure_stream(shared_buffer, state.total_lines,
                            start_line_for_cpu(main_cpu, state.total_lines));
        __atomic_fetch_add(&state.done_count, 1, __ATOMIC_ACQ_REL);
        wait_for_done(&state, active_threads);
        end = rdcycle();
        m5_dump_reset_stats(0, 0);

        total_ops = (uint64_t)active_threads * (uint64_t)state.total_lines;
        throughput = (double)total_ops / (double)(end - start);

        printf("RBOOK_LINK_PRESSURE WINDOW %d THREADS %d OPS %" PRIu64
               " CYCLES %" PRIu64 " THROUGHPUT %.6f COMPLETED %d\n",
               window, active_threads, total_ops, end - start, throughput,
               __atomic_load_n(&state.done_count, __ATOMIC_ACQUIRE));
        fflush(stdout);
    }

    for (int i = 0; i < NUM_WORKERS; ++i) {
        pthread_join(threads[i], NULL);
    }

    printf("PASS\n");
    fflush(stdout);
    pthread_barrier_destroy(&state.sync_barrier);
    return 0;
}
