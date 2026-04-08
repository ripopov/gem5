/*
 * rbook_test_smoke.c — Single-core smoke test for the 4x4 CHI mesh.
 *
 * Goal: verify the system boots and all 16 LLC slices are reachable
 * from a single core.
 *
 * Strategy:
 *   - All 16 CPUs start this binary.  An atomic amoswap picks one
 *     "master" core; the rest exit immediately.
 *   - The master allocates a large array (32 MiB) and reads one byte
 *     per cache line in a sequential sweep.  With 16 HN-F slices,
 *     address interleaving on bits [6:9] distributes consecutive
 *     cache lines across all 16 slices, so the sweep touches every
 *     HN-F at least once.
 *   - After the sweep, the master prints "PASS" and exits.
 *
 * Expected gem5 statistics after a successful run:
 *   - system.ruby.hnf<0-15>.cntrl.cache.m_demand_hits  > 0
 *   - system.ruby.hnf<0-15>.cntrl.cache.m_demand_misses > 0
 *   - Per-HNF access counts roughly balanced (within 2x).
 *   - No simulation errors.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_MB 32
#define ARRAY_SIZE ((size_t)ARRAY_MB * 1024 * 1024)
#define CL_SIZE 64

static volatile int32_t master_flag = -1;

static inline int32_t
amoswap(volatile int32_t *addr, int32_t val)
{
    int32_t ret;
    __asm__ __volatile__("amoswap.w %0, %2, (%1)"
                         : "=r"(ret)
                         : "r"(addr), "r"(val)
                         : "memory");
    return ret;
}

int
main(void)
{
    int32_t my_id = amoswap(&master_flag, 0);

    if (my_id != -1) {
        return 0;
    }

    printf("[smoke] master core starting, array = %d MiB\n", ARRAY_MB);

    char *array = (char *)malloc(ARRAY_SIZE);
    if (!array) {
        printf("[smoke] FAIL: malloc returned NULL\n");
        return 1;
    }
    memset(array, 0, ARRAY_SIZE);

    volatile char sink = 0;
    size_t n_lines = ARRAY_SIZE / CL_SIZE;

    for (size_t i = 0; i < n_lines; i++) {
        sink += array[i * CL_SIZE];
    }

    printf("[smoke] touched %zu cache lines, sink = %d\n", n_lines, (int)sink);
    printf("PASS\n");

    free((void *)array);
    return 0;
}
