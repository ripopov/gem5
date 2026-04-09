/*
 * rbook_test_hop_latency.c -- Stage 3b hop-distance latency test.
 *
 * Core 0 repeatedly measures loads to two cache lines chosen so their
 * physical-address bits [9:6] home them at HNF 0 and HNF 15. A 4 MiB
 * eviction sweep flushes the private L1/L2 caches between measurements while
 * keeping the lines resident in the distributed LLC slices.
 *
 * Two extra single-load probes are wrapped in m5 stats reset/dump calls so a
 * post-run script can inspect near-only and far-only network traffic.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include <gem5/m5ops.h>

#define CACHE_LINE 64
#define PAGE_BYTES 4096
#define NEAR_OFFSET 0
#define FAR_OFFSET (15 * CACHE_LINE)
#define EVICT_BYTES (4 * 1024 * 1024)
#define SAMPLES 8

static volatile uint64_t sink;

static inline uint64_t
rdcycle(void)
{
    uint64_t c;

    asm volatile("rdcycle %0" : "=r"(c));
    return c;
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
flush_private_caches(const volatile uint8_t *eviction_buffer)
{
    for (size_t i = 0; i < EVICT_BYTES; i += CACHE_LINE) {
        sink += eviction_buffer[i];
    }

    asm volatile("" ::: "memory");
}

static __attribute__((noinline)) uint64_t
measure_load_cycles(const volatile uint8_t *addr)
{
    uint64_t start;
    uint64_t end;
    uint8_t value;

    asm volatile("" ::: "memory");
    start = rdcycle();
    value = *addr;
    asm volatile("" ::: "memory");
    end = rdcycle();

    sink += value;
    return end - start;
}

static double
average(const uint64_t *samples, size_t count)
{
    uint64_t total = 0;

    for (size_t i = 0; i < count; ++i) {
        total += samples[i];
    }

    return (double)total / (double)count;
}

static void
print_samples(const char *label, const uint64_t *samples, size_t count)
{
    printf("RBOOK_HOP %s", label);
    for (size_t i = 0; i < count; ++i) {
        printf(" %" PRIu64, samples[i]);
    }
    printf("\n");
}

int
main(void)
{
    volatile uint8_t *target_page = map_anonymous_buffer(PAGE_BYTES);
    volatile uint8_t *eviction_buffer = map_anonymous_buffer(EVICT_BYTES);
    volatile uint8_t *near_addr = target_page + NEAR_OFFSET;
    volatile uint8_t *far_addr = target_page + FAR_OFFSET;
    uint64_t near_samples[SAMPLES];
    uint64_t far_samples[SAMPLES];
    uint64_t near_probe;
    uint64_t far_probe;
    double near_avg;
    double far_avg;

    fault_in_pages(target_page, PAGE_BYTES);
    fault_in_pages(eviction_buffer, EVICT_BYTES);

    /* Warm both lines into their home LLC slices before timed reloads. */
    sink += *near_addr;
    sink += *far_addr;
    flush_private_caches(eviction_buffer);

    for (size_t i = 0; i < SAMPLES; ++i) {
        flush_private_caches(eviction_buffer);
        near_samples[i] = measure_load_cycles(near_addr);

        flush_private_caches(eviction_buffer);
        far_samples[i] = measure_load_cycles(far_addr);
    }

    near_avg = average(near_samples, SAMPLES);
    far_avg = average(far_samples, SAMPLES);

    flush_private_caches(eviction_buffer);
    m5_reset_stats(0, 0);
    near_probe = measure_load_cycles(near_addr);
    m5_dump_reset_stats(0, 0);

    flush_private_caches(eviction_buffer);
    m5_reset_stats(0, 0);
    far_probe = measure_load_cycles(far_addr);
    m5_dump_reset_stats(0, 0);

    printf("RBOOK_HOP NEAR_OFFSET 0x%x\n", NEAR_OFFSET);
    printf("RBOOK_HOP FAR_OFFSET 0x%x\n", FAR_OFFSET);
    printf("RBOOK_HOP SAMPLES %d\n", SAMPLES);
    print_samples("NEAR_SAMPLES", near_samples, SAMPLES);
    print_samples("FAR_SAMPLES", far_samples, SAMPLES);
    printf("RBOOK_HOP NEAR_AVG %.2f\n", near_avg);
    printf("RBOOK_HOP FAR_AVG %.2f\n", far_avg);
    printf("RBOOK_HOP DELTA_AVG %.2f\n", far_avg - near_avg);
    printf("RBOOK_HOP NEAR_PROBE %" PRIu64 "\n", near_probe);
    printf("RBOOK_HOP FAR_PROBE %" PRIu64 "\n", far_probe);
    printf("PASS\n");
    fflush(stdout);

    return 0;
}
