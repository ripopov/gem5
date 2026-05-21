/*
 * bench.c - tiny deterministic benchmark for gem5 QEMU-CPU mode.
 *
 * Built into the initramfs (build-image.sh) as /bin/bench and started by
 * /init.  Flow that the snapshot bridge relies on:
 *
 *   1. snapshot_barrier() -- an explicit, race-free barrier.  qemu-snapshot.py
 *      sets a gdb breakpoint on this function, so QEMU halts at *exactly*
 *      this instruction: the snapshot captures bench right at the start of
 *      its CPU-bound region, with no capture-window timing race.
 *   2. the matrix multiply -- a long CPU-bound region with no system calls,
 *      continued purely by instruction execution after a snapshot restore.
 *   3. report the checksum on the console with write(2).  After the device
 *      state (PLIC, UART) is restored this reaches gem5's terminal normally;
 *      bench then calls m5_exit only to end the simulation cleanly.
 */
#include <unistd.h>
#include <stdint.h>
#include <termios.h>

#define N 64
#define REPS 8
#define EXPECTED_SUM 3210805248ULL   /* verified under QEMU */

static uint64_t a[N][N], b[N][N], c[N][N];

/*
 * The explicit snapshot barrier.  Kept in its own non-inlined function so it
 * has a stable symbol address for qemu-snapshot.py to breakpoint on.
 */
__attribute__((noinline)) void
snapshot_barrier(void)
{
    __asm__ volatile("nop" ::: "memory");
}

/* gem5 m5ops: instruction = 0x0000007b | (func << 25). */
static inline void
m5_exit(uint64_t delay)
{
    register uint64_t a0 __asm__("a0") = delay;
    __asm__ volatile(".word 0x4200007b" : : "r"(a0) : "memory");
}

/* Append an unsigned decimal number to p, return the new end pointer. */
static char *
u64_to_dec(char *p, uint64_t v)
{
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n)
        *p++ = tmp[--n];
    return p;
}

int
main(void)
{
    write(1, "QEMU-CPU-MODE-BENCH-READY\n", 26);

    /* (1) Race-free snapshot point. */
    snapshot_barrier();

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

    /* (3) Report on the console - works once PLIC/UART state is restored. */
    char msg[64];
    char *p = msg;
    const char *tag = (sum == EXPECTED_SUM) ? "BENCH-DONE ok sum="
                                            : "BENCH-DONE BAD sum=";
    while (*tag)
        *p++ = *tag++;
    p = u64_to_dec(p, sum);
    *p++ = '\n';
    write(1, msg, (size_t)(p - msg));

    /* Wait for the console output to be fully transmitted by the (now
     * interrupt-driven) UART before ending the simulation. */
    tcdrain(1);

    /* End the gem5 simulation cleanly. */
    m5_exit(0);
    return 0;
}
