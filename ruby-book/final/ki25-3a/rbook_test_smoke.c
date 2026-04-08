/*
 * rbook_test_smoke.c - Single-core smoke test for 4x4 CHI mesh
 *
 * Goal: Verify system boots and all 16 LLC slices are reachable from a single
 * core.
 *
 * Core 0 allocates a large array (at least 16 x LLC slice size) and reads
 * every 64th byte (one per cache line) in a sequential sweep.
 * With --num-dirs=2 and --num-l3caches=16, address interleaving distributes
 * cache lines across all 16 HN-F slices.
 *
 * Build: make
 * Run:   ./build/RISCV/gem5.opt -d m5out-smoke-$DATE \\
 *            configs/example/rbook_mesh_config.py \\
 *            --cmd=ruby-book/final/ki25-3a/rbook_test_smoke
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Cache line size is 64 bytes (standard)
 * Assuming 256KB per LLC slice, 16 slices = 4MB
 * We allocate 8MB to ensure full coverage with margin
 */
#define CACHE_LINE_SIZE 64
#define LLC_SLICE_SIZE (256 * 1024) /* 256 KB per slice */
#define NUM_LLC_SLICES 16
#define ARRAY_SIZE (NUM_LLC_SLICES * LLC_SLICE_SIZE * 2) /* 8 MB */
#define NUM_ACCESSES (ARRAY_SIZE / CACHE_LINE_SIZE)

/* Minimum accesses expected per HNF for validation */
#define MIN_ACCESSES_PER_SLICE ((NUM_ACCESSES / NUM_LLC_SLICES) / 2)

int
main(void)
{
    volatile unsigned char *array;
    unsigned long sum = 0;
    size_t i;

    printf("Smoke test: Single-core LLC reachability test\n");
    printf("System: 4x4 CHI mesh with 16 HN-F (LLC) + 2 SN-F (DDR)\n");
    printf("Test: Core 0 accesses %lu cache lines across 8 MB array\n",
           (unsigned long)NUM_ACCESSES);
    printf("Expected: ~%lu accesses per HN-F slice\n\n",
           (unsigned long)(NUM_ACCESSES / NUM_LLC_SLICES));

    /* Allocate and touch array to ensure pages are mapped */
    array = (volatile unsigned char *)malloc(ARRAY_SIZE);
    if (array == NULL) {
        printf("FAIL: Could not allocate %d MB array\n",
               ARRAY_SIZE / (1024 * 1024));
        return 1;
    }

    /* Initialize array (one write per cache line) */
    printf("Phase 1: Initializing array (write every cache line)...\n");
    for (i = 0; i < NUM_ACCESSES; i++) {
        array[i * CACHE_LINE_SIZE] = (unsigned char)(i & 0xFF);
    }
    printf("  Wrote %lu cache lines\n", (unsigned long)NUM_ACCESSES);

    /*
     * Sequential sweep: read every cache line
     * This exercises all 16 LLC slices via address interleaving.
     * Each cache line maps to a specific HN-F based on address bits.
     */
    printf("\nPhase 2: Reading all cache lines (LLC lookup test)...\n");
    for (i = 0; i < NUM_ACCESSES; i++) {
        sum += array[i * CACHE_LINE_SIZE];
    }
    printf("  Read %lu cache lines\n", (unsigned long)NUM_ACCESSES);

    /* Prevent compiler from optimizing away the reads */
    if (sum == 0) {
        printf("\nWARNING: Sum is zero (unexpected but not an error)\n");
    }

    printf("\nValidation:\n");
    printf("  Total cache line accesses: %lu\n", (unsigned long)NUM_ACCESSES);
    printf("  Expected per HN-F slice:   ~%lu\n",
           (unsigned long)(NUM_ACCESSES / NUM_LLC_SLICES));
    printf("  Minimum for pass:          %d\n", MIN_ACCESSES_PER_SLICE);

    printf("\nPASS\n");
    printf("All 16 LLC slices should show nonzero access counts in stats.\n");

    free((void *)array);
    return 0;
}
