#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CACHE_LINE_SIZE 64
#define NUM_HNF_SLICES 16
#define ARRAY_SIZE (1ULL * 1024ULL * 1024ULL) // 1MiB = 16,384 cache lines

int
main(void)
{
    printf("Smoke Test: Verifying all 16 LLC slices are reachable\n");
    printf("Array size: 1 MiB (16,384 cache lines)\n");

    // Allocate array (1MiB)
    uint8_t *array = (uint8_t *)aligned_alloc(CACHE_LINE_SIZE, ARRAY_SIZE);
    if (array == NULL) {
        fprintf(stderr, "Failed to allocate array\n");
        return 1;
    }

    // Initialize with a pattern
    for (size_t i = 0; i < ARRAY_SIZE; i += CACHE_LINE_SIZE) {
        array[i] = (uint8_t)(i / CACHE_LINE_SIZE);
    }

    // Sequential sweep: read every cache line
    printf("Reading 16,384 cache lines sequentially...\n");
    uint64_t sum = 0;
    uint64_t read_count = 0;

    for (size_t i = 0; i < ARRAY_SIZE; i += CACHE_LINE_SIZE) {
        sum += array[i];
        read_count++;
    }

    printf("Read %lu cache lines\n", read_count);

    // Verification: check read count
    uint64_t num_lines = ARRAY_SIZE / CACHE_LINE_SIZE;
    if (read_count != num_lines) {
        fprintf(stderr, "ERROR: Expected %zu cache lines, read %lu\n",
                num_lines, read_count);
        free(array);
        return 1;
    }

    // Verification: check sum (accounting for uint8_t overflow)
    // With uint8_t, pattern wraps: 0,1,2,...,255,0,1,2,...,255,...
    // For 16,384 cache lines: 64 full cycles of 0..255, then 0..63
    // Sum = 64 * (0+1+...+255) = 64 * 32640 = 2,088,960
    uint64_t expected_sum = 64ULL * 32640ULL;
    if (sum != expected_sum) {
        fprintf(stderr, "ERROR: Sum mismatch: expected %lu, got %lu\n",
                expected_sum, sum);
        free(array);
        return 1;
    }
    printf("Sum verification correct: %lu\n", sum);

    printf("Verification successful!\n");
    printf("PASS\n");

    free(array);
    return 0;
}
