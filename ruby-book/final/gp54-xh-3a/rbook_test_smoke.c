#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gem5/m5ops.h>

#ifndef RBOOK_L3_SLICE_BYTES
#define RBOOK_L3_SLICE_BYTES (512u * 1024u)
#endif

#ifndef RBOOK_SWEEP_PASSES
#define RBOOK_SWEEP_PASSES 2u
#endif

#define RBOOK_NUM_LLC_SLICES 16u
#define RBOOK_CACHE_LINE_BYTES 64u
#define RBOOK_ARRAY_BYTES                                                     \
    ((size_t)RBOOK_NUM_LLC_SLICES * (size_t)RBOOK_L3_SLICE_BYTES)

static uint8_t
line_value(size_t line_index)
{
    return (uint8_t)(line_index & 0xffu);
}

int
main(void)
{
    uint8_t *buffer = NULL;
    const size_t total_lines = RBOOK_ARRAY_BYTES / RBOOK_CACHE_LINE_BYTES;
    uint64_t expected_per_pass = 0;
    volatile uint64_t observed_sum = 0;

    if ((RBOOK_ARRAY_BYTES % RBOOK_CACHE_LINE_BYTES) != 0) {
        fprintf(stderr, "array size must be cache-line aligned\n");
        return 1;
    }

    const int alloc_status = posix_memalign(
        (void **)&buffer, RBOOK_CACHE_LINE_BYTES, RBOOK_ARRAY_BYTES);
    if (alloc_status != 0) {
        fprintf(stderr, "posix_memalign failed: %s\n", strerror(alloc_status));
        return 1;
    }

    for (size_t line = 0; line < total_lines; ++line) {
        const uint8_t value = line_value(line);
        buffer[line * RBOOK_CACHE_LINE_BYTES] = value;
        expected_per_pass += value;
    }

    m5_reset_stats(0, 0);

    for (size_t pass = 0; pass < RBOOK_SWEEP_PASSES; ++pass) {
        for (size_t line = 0; line < total_lines; ++line) {
            observed_sum += buffer[line * RBOOK_CACHE_LINE_BYTES];
        }
    }

    m5_dump_stats(0, 0);

    const uint64_t expected_sum = expected_per_pass * RBOOK_SWEEP_PASSES;

    if (observed_sum != expected_sum) {
        fprintf(stderr, "FAIL expected=%" PRIu64 " observed=%" PRIu64 "\n",
                expected_sum, observed_sum);
        free(buffer);
        return 1;
    }

    printf("PASS array_bytes=%zu lines=%zu passes=%u slice_bytes=%u\n",
           RBOOK_ARRAY_BYTES, total_lines, RBOOK_SWEEP_PASSES,
           (unsigned)RBOOK_L3_SLICE_BYTES);

    free(buffer);
    return 0;
}
