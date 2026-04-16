/*
 * rbook_test_hotspot_heavy.c -- Single-thread remote-HNF rate probe.
 *
 * This checkpoint narrows hotspot-heavy to one measured CPU: main() on CPU 0.
 * The benchmark issues private accesses to cache lines that all home at HNF15
 * and reports the resulting cache-line operation rate. The near-term goal is
 * to maximize remote-HNF cache-line rate by changing the stream shape and, if
 * useful, widening the O3 core structures that cap miss concurrency.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <gem5/m5ops.h>

#define PAGE_BYTES 4096
#define HOT_OFFSET (15 * 64)
#define HOT_LINE_STRIDE (16 * 64)
#define HOT_LINES_PER_PAGE 4
#define DEFAULT_STREAM_PAGES 2048
#define MAX_STREAM_PAGES 16384
#define DEFAULT_READ_STREAMS 1
#define DEFAULT_WRITE_STREAMS 0
#define MAX_STREAMS 16
#define STREAM_LANES 8

static volatile uint64_t sink;

struct StreamConfig
{
    int stream_pages;
    int read_streams;
    int write_streams;
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
parse_positive_int(const char *text, int max_value, const char *name)
{
    char *end = NULL;
    long value = strtol(text, &end, 0);

    if (end == text || *end != '\0' || value <= 0 || value > max_value) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(1);
    }

    return (int)value;
}

static int
parse_non_negative_int(const char *text, int max_value, const char *name)
{
    char *end = NULL;
    long value = strtol(text, &end, 0);

    if (end == text || *end != '\0' || value < 0 || value > max_value) {
        fprintf(stderr, "invalid %s: %s\n", name, text);
        exit(1);
    }

    return (int)value;
}

static struct StreamConfig
parse_stream_config(int argc, char **argv)
{
    struct StreamConfig config;

    config.stream_pages = DEFAULT_STREAM_PAGES;
    config.read_streams = DEFAULT_READ_STREAMS;
    config.write_streams = DEFAULT_WRITE_STREAMS;

    if (argc == 1) {
        return config;
    }
    if (argc != 4) {
        fprintf(stderr,
                "usage: %s [stream-pages read-streams write-streams]\n",
                argv[0]);
        exit(1);
    }

    config.stream_pages =
        parse_positive_int(argv[1], MAX_STREAM_PAGES, "stream page count");
    config.read_streams =
        parse_non_negative_int(argv[2], MAX_STREAMS, "read stream count");
    config.write_streams =
        parse_non_negative_int(argv[3], MAX_STREAMS, "write stream count");

    if (config.read_streams + config.write_streams <= 0) {
        fail("at least one read or write stream is required");
    }

    return config;
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

static inline void
touch_read_lines(const volatile uint64_t *buf, int page, uint64_t *acc)
{
    *acc += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                   (size_t)page * PAGE_BYTES + HOT_OFFSET);
    *acc += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                   (size_t)page * PAGE_BYTES + HOT_OFFSET +
                                   HOT_LINE_STRIDE);
    *acc += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                   (size_t)page * PAGE_BYTES + HOT_OFFSET +
                                   2 * HOT_LINE_STRIDE);
    *acc += *(volatile uint64_t *)((volatile uint8_t *)buf +
                                   (size_t)page * PAGE_BYTES + HOT_OFFSET +
                                   3 * HOT_LINE_STRIDE);
}

static inline void
touch_write_lines(volatile uint64_t *buf, int page, uint64_t seed)
{
    *(volatile uint64_t *)((volatile uint8_t *)buf +
                           (size_t)page * PAGE_BYTES + HOT_OFFSET) = seed;
    *(volatile uint64_t *)((volatile uint8_t *)buf +
                           (size_t)page * PAGE_BYTES + HOT_OFFSET +
                           HOT_LINE_STRIDE) = seed + 1;
    *(volatile uint64_t *)((volatile uint8_t *)buf +
                           (size_t)page * PAGE_BYTES + HOT_OFFSET +
                           2 * HOT_LINE_STRIDE) = seed + 2;
    *(volatile uint64_t *)((volatile uint8_t *)buf +
                           (size_t)page * PAGE_BYTES + HOT_OFFSET +
                           3 * HOT_LINE_STRIDE) = seed + 3;
}

static __attribute__((noinline)) void
run_single_read_stream(const volatile uint64_t *buf, int stream_pages)
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
            touch_read_lines(buf, page + 0, &acc0);
        }
        if (page + 1 < stream_pages) {
            touch_read_lines(buf, page + 1, &acc1);
        }
        if (page + 2 < stream_pages) {
            touch_read_lines(buf, page + 2, &acc2);
        }
        if (page + 3 < stream_pages) {
            touch_read_lines(buf, page + 3, &acc3);
        }
        if (page + 4 < stream_pages) {
            touch_read_lines(buf, page + 4, &acc4);
        }
        if (page + 5 < stream_pages) {
            touch_read_lines(buf, page + 5, &acc5);
        }
        if (page + 6 < stream_pages) {
            touch_read_lines(buf, page + 6, &acc6);
        }
        if (page + 7 < stream_pages) {
            touch_read_lines(buf, page + 7, &acc7);
        }
    }

    sink += acc0 + acc1 + acc2 + acc3 + acc4 + acc5 + acc6 + acc7;
    asm volatile("" ::: "memory");
}

static __attribute__((noinline)) void
run_hotspot_heavy_stream(volatile uint64_t **read_bufs, int read_streams,
                         volatile uint64_t **write_bufs, int write_streams,
                         int stream_pages)
{
    if (read_streams == 1 && write_streams == 0) {
        run_single_read_stream(read_bufs[0], stream_pages);
        return;
    }

    uint64_t acc[STREAM_LANES] = {0};
    uint64_t store_seed = 0x9e3779b97f4a7c15ULL;

    for (int page = 0; page < stream_pages; page += STREAM_LANES) {
        for (int lane = 0; lane < STREAM_LANES; ++lane) {
            int current_page = page + lane;

            if (current_page >= stream_pages) {
                continue;
            }

            for (int stream = 0; stream < read_streams; ++stream) {
                touch_read_lines(read_bufs[stream], current_page, &acc[lane]);
            }

            for (int stream = 0; stream < write_streams; ++stream) {
                uint64_t seed = store_seed + ((uint64_t)stream << 40) +
                                ((uint64_t)current_page << 4);

                touch_write_lines(write_bufs[stream], current_page, seed);
                store_seed += 0x100000001b3ULL;
            }
        }
    }

    for (int lane = 0; lane < STREAM_LANES; ++lane) {
        sink += acc[lane];
    }
    sink += store_seed;
    asm volatile("" ::: "memory");
}

int
main(int argc, char **argv)
{
    struct StreamConfig config;
    size_t stream_bytes;
    volatile uint64_t *read_streams[MAX_STREAMS] = {0};
    volatile uint64_t *write_streams[MAX_STREAMS] = {0};
    uint64_t start;
    uint64_t end;
    uint64_t total_ops;
    int main_cpu;

    config = parse_stream_config(argc, argv);
    main_cpu = gem5_cpu_id();
    if (main_cpu != 0) {
        fprintf(stderr, "FAIL: expected main on CPU 0, got %d\n", main_cpu);
        return 1;
    }

    stream_bytes = (size_t)config.stream_pages * PAGE_BYTES;
    for (int i = 0; i < config.read_streams; ++i) {
        read_streams[i] = map_anonymous_buffer(stream_bytes);
        fault_in_pages(read_streams[i], stream_bytes);
    }
    for (int i = 0; i < config.write_streams; ++i) {
        write_streams[i] = map_anonymous_buffer(stream_bytes);
        fault_in_pages(write_streams[i], stream_bytes);
    }

    printf("RBOOK_HOTSPOT_HEAVY MAIN_CPU %d\n", main_cpu);
    printf("RBOOK_HOTSPOT_HEAVY STREAM_PAGES %d\n", config.stream_pages);
    printf("RBOOK_HOTSPOT_HEAVY READ_STREAMS %d\n", config.read_streams);
    printf("RBOOK_HOTSPOT_HEAVY WRITE_STREAMS %d\n", config.write_streams);
    printf("RBOOK_HOTSPOT_HEAVY HOT_OFFSET 0x%x\n", HOT_OFFSET);
    printf("RBOOK_HOTSPOT_HEAVY HOT_LINES_PER_PAGE %d\n", HOT_LINES_PER_PAGE);
    fflush(stdout);

    m5_reset_stats(0, 0);
    start = rdcycle();
    run_hotspot_heavy_stream(read_streams, config.read_streams, write_streams,
                             config.write_streams, config.stream_pages);
    end = rdcycle();
    m5_dump_reset_stats(0, 0);

    total_ops = (uint64_t)config.stream_pages * HOT_LINES_PER_PAGE *
                (uint64_t)(config.read_streams + config.write_streams);

    printf("RBOOK_HOTSPOT_HEAVY WINDOW 0 THREADS 1 OPS %" PRIu64
           " CYCLES %" PRIu64 " THROUGHPUT %.6f COMPLETED 1\n",
           total_ops, end - start, (double)total_ops / (double)(end - start));
    printf("PASS\n");
    fflush(stdout);
    return 0;
}
