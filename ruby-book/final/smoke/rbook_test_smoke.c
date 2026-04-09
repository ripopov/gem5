/*
 * rbook_test_smoke.c — Single-core smoke test (Stage 3a)
 *
 * Verifies the 4x4 CHI mesh boots and all 16 LLC (HN-F) slices are
 * reachable from a single core.
 *
 * Core 0 sweeps a large array reading one byte per cache line.  With
 * 16 HN-F slices interleaved on address bits [9:6], sequential
 * cache-line accesses cycle through all 16 slices every 16 lines.
 *
 * After the sweep, check per-HN-F stats (m_demand_hits + m_demand_misses
 * across L3Cache_Controller instances) — all 16 should be nonzero and
 * roughly balanced.
 */

#include <stdint.h>
#include <stdio.h>

/*
 * 1 MiB gives 16384 cache lines.  With 16-way HN-F interleaving that
 * is ~1024 lines per slice — enough for clearly nonzero per-slice counts
 * while keeping simulation time reasonable.
 */
#define ARRAY_SIZE (1 << 20) /* 1 MiB */
#define CACHE_LINE 64

/*
 * volatile marks each access as a side effect that the compiler must
 * not optimise away, reorder, or merge (C11 §6.7.3).  In practice
 * GCC/Clang emit a real load instruction per read, which is what we
 * need so every cache-line access actually travels the memory hierarchy.
 */
static volatile char array[ARRAY_SIZE];

int
main(void)
{
    volatile char sink = 0;

    /* Sequential sweep: read one byte per cache line. */
    for (size_t off = 0; off < ARRAY_SIZE; off += CACHE_LINE) {
        sink = array[off];
    }

    (void)sink;
    printf("PASS\n");
    return 0;
}
