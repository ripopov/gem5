/*
 * bench.c - tiny deterministic benchmark for gem5 QEMU-CPU mode.
 *
 * Built into the initramfs (build-image.sh) as /bin/bench and started by
 * /init.  Flow that the snapshot bridge relies on:
 *
 *   1. print BENCH_READY  -- qemu-snapshot.py waits for this, then stops
 *      the VM *immediately*, so the snapshot catches this process right
 *      at the start of the matrix-multiply loop;
 *   2. the matrix multiply -- a long CPU-bound region with no system
 *      calls, so it continues purely by instruction execution after the
 *      snapshot is restored into gem5's detailed CPU;
 *   3. signal completion to gem5 with an m5op: m5_exit if the checksum is
 *      correct, m5_fail otherwise.  m5ops are used (rather than printing
 *      to the console) because after a snapshot restore the interrupt-
 *      driven UART path is not wired up, whereas gem5 decodes m5ops
 *      directly - so this gives an unambiguous pass/fail.
 *
 * Under plain QEMU the trailing m5op is an illegal instruction; that is
 * harmless because qemu-snapshot.py always snapshots before it runs.
 */
#include <unistd.h>
#include <stdint.h>

#define N 64
#define REPS 8
#define EXPECTED_SUM 3210805248ULL   /* verified under QEMU */

static uint64_t a[N][N], b[N][N], c[N][N];

/* gem5 m5ops: instruction = 0x0000007b | (func << 25). */
static inline void
m5_exit(uint64_t delay)
{
    register uint64_t a0 __asm__("a0") = delay;
    __asm__ volatile(".word 0x4200007b" : : "r"(a0) : "memory");
}

static inline void
m5_fail(uint64_t delay, uint64_t code)
{
    register uint64_t a0 __asm__("a0") = delay;
    register uint64_t a1 __asm__("a1") = code;
    __asm__ volatile(".word 0x4400007b" : : "r"(a0), "r"(a1) : "memory");
}

int
main(void)
{
    /* (1) Snapshot point: qemu-snapshot.py stops the VM right here. */
    write(1, "QEMU-CPU-MODE-BENCH-READY\n", 26);

    /* (2) CPU-bound region - no syscalls until it finishes. */
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            a[i][j] = (uint64_t)i * 3 + j;
            b[i][j] = (uint64_t)i + j * 2;
        }

    for (int r = 0; r < REPS; r++)
        for (int i = 0; i < N; i++)
            for (int j = 0; j < N; j++) {
                uint64_t s = 0;
                for (int k = 0; k < N; k++)
                    s += a[i][k] * b[k][j];
                c[i][j] = s;
            }

    uint64_t sum = 0;
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++)
            sum += c[i][j];

    /* (3) Tell gem5 whether the detailed run reproduced the checksum. */
    if (sum == EXPECTED_SUM)
        m5_exit(0);
    else
        m5_fail(0, sum);

    return 0;
}
