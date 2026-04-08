/*
 * rbook_test_smoke.c — Single-core smoke test (Chapter 17, Stage 3a)
 *
 * Verifies that all 16 LLC slices (HN-F) in the 4x4 CHI mesh are
 * reachable.  Core 0 sweeps a large array one cache line at a time;
 * address interleaving (bits [9:6] select among 16 HN-F slices)
 * distributes the accesses across every slice.
 *
 * Success criteria (checked in stats):
 *   - Simulation completes and prints "PASS".
 *   - Every L3Cache_Controller shows nonzero m_demand_hits + m_demand_misses.
 *   - Access counts are roughly balanced across the 16 slices.
 *
 * Build:  riscv64-linux-gnu-gcc -O2 -static -o rbook_test_smoke
 * rbook_test_smoke.c
 * Run:    ./build/RISCV/gem5.opt -d m5out/rbook-smoke-<ts> \
 *             configs/example/rbook_mesh_config.py \
 *             --cmd=ruby-book/final/cco46-3a/rbook_test_smoke
 */

#include <stdint.h>
#include <stdio.h>

/*
 * 4 MiB array — larger than the default L2 (2 MiB) so every access
 * misses in L1+L2 and reaches the L3 (HN-F).  With 64-byte stride
 * that is 65 536 cache-line touches, well distributed across 16 slices
 * (~4 096 per slice).
 */
#define ARRAY_SIZE (4 * 1024 * 1024)
#define CACHE_LINE 64

static char array[ARRAY_SIZE] __attribute__((aligned(CACHE_LINE)));

int
main(void)
{
    /*
     * Use a volatile pointer so the compiler cannot elide the loads
     * even at -O2.  Each iteration touches one byte per cache line,
     * pulling the entire line into the hierarchy.
     */
    volatile char *p = (volatile char *)array;
    volatile char sink = 0;

    for (int i = 0; i < ARRAY_SIZE; i += CACHE_LINE) {
        sink = p[i];
    }

    (void)sink;
    printf("PASS\n");
    return 0;
}
