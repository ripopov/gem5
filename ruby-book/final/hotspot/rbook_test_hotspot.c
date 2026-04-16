/*
 * rbook_test_hotspot.c -- Stage 3f hotspot / backpressure benchmark.
 *
 * The benchmark sweeps 1, 2, 4, 8, and 16 active CPUs.
 * Each active CPU streams one private cache line per page from offset 0x3c0,
 * which homes every measured line at HNF15 in the Chapter 17 mesh config.
 *
 * The measured windows are wrapped in m5_reset_stats()/m5_dump_reset_stats()
 * so the checker can examine one isolated stats block per offered-load point.
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
#define HOT_OFFSET (15 * CACHE_LINE)
#define HOT_LINE_STRIDE (16 * CACHE_LINE)
#define HOT_LINES_PER_PAGE 4
#define DEFAULT_STREAM_PAGES 2048
#define MAX_STREAM_PAGES 16384
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
fault_in_pages(volatile uint64_t *buf, size_t size)
{
    for (size_t i = 0; i < size; i += PAGE_BYTES) {
        sink += *(volatile uint64_t *)((volatile uint8_t *)buf + i);
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

static __attribute__((noinline)) void
run_hotspot_stream(const volatile uint64_t *buf, int stream_pages)
{
    uint64_t acc0 = 0;
    uint64_t acc1 = 0;
    uint64_t acc2 = 0;
    uint64_t acc3 = 0;
    uint64_t acc4 = 0;
    uint64_t acc5 = 0;
    uint64_t acc6 = 0;
    uint64_t acc7 = 0;

    for (int page = 0; page < stream_pages; page += STREAM_LANES) {
        if (page + 0 < stream_pages) {
            acc0 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 0) * PAGE_BYTES +
                                           HOT_OFFSET);
            acc0 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 0) * PAGE_BYTES +
                                           HOT_OFFSET + HOT_LINE_STRIDE);
            acc0 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 0) * PAGE_BYTES +
                                           HOT_OFFSET + 2 * HOT_LINE_STRIDE);
            acc0 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 0) * PAGE_BYTES +
                                           HOT_OFFSET + 3 * HOT_LINE_STRIDE);
        }
        if (page + 1 < stream_pages) {
            acc1 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 1) * PAGE_BYTES +
                                           HOT_OFFSET);
            acc1 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 1) * PAGE_BYTES +
                                           HOT_OFFSET + HOT_LINE_STRIDE);
            acc1 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 1) * PAGE_BYTES +
                                           HOT_OFFSET + 2 * HOT_LINE_STRIDE);
            acc1 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 1) * PAGE_BYTES +
                                           HOT_OFFSET + 3 * HOT_LINE_STRIDE);
        }
        if (page + 2 < stream_pages) {
            acc2 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 2) * PAGE_BYTES +
                                           HOT_OFFSET);
            acc2 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 2) * PAGE_BYTES +
                                           HOT_OFFSET + HOT_LINE_STRIDE);
            acc2 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 2) * PAGE_BYTES +
                                           HOT_OFFSET + 2 * HOT_LINE_STRIDE);
            acc2 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 2) * PAGE_BYTES +
                                           HOT_OFFSET + 3 * HOT_LINE_STRIDE);
        }
        if (page + 3 < stream_pages) {
            acc3 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 3) * PAGE_BYTES +
                                           HOT_OFFSET);
            acc3 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 3) * PAGE_BYTES +
                                           HOT_OFFSET + HOT_LINE_STRIDE);
            acc3 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 3) * PAGE_BYTES +
                                           HOT_OFFSET + 2 * HOT_LINE_STRIDE);
            acc3 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 3) * PAGE_BYTES +
                                           HOT_OFFSET + 3 * HOT_LINE_STRIDE);
        }
        if (page + 4 < stream_pages) {
            acc4 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 4) * PAGE_BYTES +
                                           HOT_OFFSET);
            acc4 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 4) * PAGE_BYTES +
                                           HOT_OFFSET + HOT_LINE_STRIDE);
            acc4 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 4) * PAGE_BYTES +
                                           HOT_OFFSET + 2 * HOT_LINE_STRIDE);
            acc4 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 4) * PAGE_BYTES +
                                           HOT_OFFSET + 3 * HOT_LINE_STRIDE);
        }
        if (page + 5 < stream_pages) {
            acc5 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 5) * PAGE_BYTES +
                                           HOT_OFFSET);
            acc5 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 5) * PAGE_BYTES +
                                           HOT_OFFSET + HOT_LINE_STRIDE);
            acc5 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 5) * PAGE_BYTES +
                                           HOT_OFFSET + 2 * HOT_LINE_STRIDE);
            acc5 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 5) * PAGE_BYTES +
                                           HOT_OFFSET + 3 * HOT_LINE_STRIDE);
        }
        if (page + 6 < stream_pages) {
            acc6 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 6) * PAGE_BYTES +
                                           HOT_OFFSET);
            acc6 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 6) * PAGE_BYTES +
                                           HOT_OFFSET + HOT_LINE_STRIDE);
            acc6 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 6) * PAGE_BYTES +
                                           HOT_OFFSET + 2 * HOT_LINE_STRIDE);
            acc6 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 6) * PAGE_BYTES +
                                           HOT_OFFSET + 3 * HOT_LINE_STRIDE);
        }
        if (page + 7 < stream_pages) {
            acc7 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 7) * PAGE_BYTES +
                                           HOT_OFFSET);
            acc7 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 7) * PAGE_BYTES +
                                           HOT_OFFSET + HOT_LINE_STRIDE);
            acc7 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 7) * PAGE_BYTES +
                                           HOT_OFFSET + 2 * HOT_LINE_STRIDE);
            acc7 += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                           (size_t)(page + 7) * PAGE_BYTES +
                                           HOT_OFFSET + 3 * HOT_LINE_STRIDE);
        }
    }

    sink += acc0 + acc1 + acc2 + acc3 + acc4 + acc5 + acc6 + acc7;
    asm volatile("" ::: "memory");
}

static void *
worker_main(void *arg)
{
    struct WorkerArg *worker = arg;
    struct SharedState *state = worker->state;
    size_t stream_bytes = (size_t)state->stream_pages * PAGE_BYTES;
    volatile uint64_t *stream = map_anonymous_buffer(stream_bytes);
    int cpu = gem5_cpu_id();

    if (cpu < 0) {
        fail("worker getcpu failed");
    }
    if (cpu <= 0 || cpu >= NUM_CPUS) {
        fprintf(stderr, "FAIL: unexpected worker CPU id %d\n", cpu);
        exit(1);
    }

    fault_in_pages(stream, stream_bytes);
    state->worker_cpus[worker->worker_index] = cpu;
    __atomic_store_n(&state->worker_seen[worker->worker_index], 1,
                     __ATOMIC_RELEASE);

    wait_barrier(&state->sync_barrier, "setup barrier");

    for (int window = 0; window < NUM_WINDOWS; ++window) {
        wait_barrier(&state->sync_barrier, "window barrier");
        wait_for_window(state, window + 1);

        if (cpu < __atomic_load_n(&state->active_threads, __ATOMIC_ACQUIRE)) {
            run_hotspot_stream(stream, state->stream_pages);
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
    size_t stream_bytes;
    volatile uint64_t *main_stream;
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

    stream_bytes = (size_t)state.stream_pages * PAGE_BYTES;
    main_stream = map_anonymous_buffer(stream_bytes);
    fault_in_pages(main_stream, stream_bytes);

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

    printf("RBOOK_HOTSPOT MAIN_CPU %d\n", main_cpu);
    printf("RBOOK_HOTSPOT STREAM_PAGES %d\n", state.stream_pages);
    printf("RBOOK_HOTSPOT HOT_OFFSET 0x%x\n", HOT_OFFSET);
    printf("RBOOK_HOTSPOT HOT_LINES_PER_PAGE %d\n", HOT_LINES_PER_PAGE);
    for (int i = 0; i < NUM_WORKERS; ++i) {
        printf("RBOOK_HOTSPOT WORKER_%02d_CPU %d\n", i + 1,
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

        run_hotspot_stream(main_stream, state.stream_pages);
        __atomic_fetch_add(&state.done_count, 1, __ATOMIC_ACQ_REL);
        wait_for_done(&state, active_threads);
        end = rdcycle();
        m5_dump_reset_stats(0, 0);

        total_ops = (uint64_t)active_threads * (uint64_t)state.stream_pages *
                    HOT_LINES_PER_PAGE;
        throughput = (double)total_ops / (double)(end - start);

        printf("RBOOK_HOTSPOT WINDOW %d THREADS %d OPS %" PRIu64
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
