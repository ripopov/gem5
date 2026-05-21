/*
 * philo.c - dining-philosophers benchmark for gem5 QEMU-CPU mode.
 *
 * A genuinely multi-threaded, multicore workload used to validate that a
 * snapshot of an SMP RISC-V system restores and runs correctly on gem5's
 * detailed CPU models.  NPHIL philosopher threads contend for shared forks
 * (pthread mutexes) for a fixed number of rounds; the kernel schedules them
 * across every hart, so the restored run exercises:
 *
 *   - per-hart register / CSR restore for several harts at once,
 *   - cross-hart wakeups (futex -> IPI over the CLINT software interrupt),
 *   - the SMP scheduler migrating threads between harts.
 *
 * Determinism: regardless of how the philosophers interleave, every one of
 * them eats exactly ROUNDS times and the checksum (sum of the eater's id
 * over every meal) is fixed.  main() validates both and reports the result
 * on the console, so the scenario can be checked end to end.
 *
 * Flow the snapshot bridge relies on:
 *   1. snapshot_barrier() -- the race-free capture point.  qemu-snapshot.py
 *      sets a gdb breakpoint here, so QEMU halts at *exactly* this
 *      instruction.  The philosopher threads have already been created, so
 *      the captured snapshot is of a live multi-threaded SMP process and the
 *      whole contended dining region then runs under gem5.
 *   2. join every philosopher -- the multicore region: mutex contention,
 *      blocking and cross-hart wakeups, continued after the restore.
 *   3. validate the deterministic result and report it on the console with
 *      write(2); m5_exit only ends the simulation cleanly afterwards.
 */
#include <pthread.h>
#include <stdint.h>
#include <termios.h>
#include <unistd.h>

#define NPHIL  5             /* philosophers / forks (classic problem)      */
#define ROUNDS 64            /* meals each philosopher must eat             */
#define WORK   256           /* deterministic CPU work per think/eat phase  */

/* Every philosopher eats ROUNDS times. */
#define EXPECT_TOTAL ((uint64_t)NPHIL * ROUNDS)
/* Each meal adds the eater's id; sum of ids 0..NPHIL-1 = NPHIL*(NPHIL-1)/2. */
#define EXPECT_CHECK ((uint64_t)ROUNDS * (NPHIL * (NPHIL - 1) / 2))

static pthread_mutex_t fork_mtx[NPHIL];
static pthread_mutex_t stats_mtx = PTHREAD_MUTEX_INITIALIZER;

static uint64_t meals[NPHIL]; /* per-philosopher meal count                 */
static uint64_t checksum;     /* sum of the eater's id over every meal      */
static uint64_t work_acc;     /* keeps the think/eat work observable        */

/*
 * The explicit snapshot barrier.  Kept in its own non-inlined function so it
 * has a stable symbol address for qemu-snapshot.py to breakpoint on.
 */
__attribute__((noinline)) void
snapshot_barrier(void)
{
    __asm__ volatile("nop" ::: "memory");
}

/* gem5 m5ops: instruction = 0x0000007b | (func << 25); func 0x21 = m5_exit. */
static inline void
m5_exit(uint64_t delay)
{
    register uint64_t a0 __asm__("a0") = delay;
    __asm__ volatile(".word 0x4200007b" : : "r"(a0) : "memory");
}

/* A small deterministic busy loop, so "think" and "eat" cost real cycles. */
static uint64_t
spin_work(uint64_t seed)
{
    for (int i = 0; i < WORK; i++)
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return seed;
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

static void *
philosopher(void *arg)
{
    long id = (long)arg;
    int left = (int)id;
    int right = (int)((id + 1) % NPHIL);
    uint64_t local = (uint64_t)id + 1;

    for (int r = 0; r < ROUNDS; r++) {
        /* Think. */
        local = spin_work(local);

        /* Pick up both forks.  The last philosopher takes them in the
         * opposite order -- the classic asymmetric, deadlock-free
         * solution -- which still produces real contention and blocking. */
        if (id == NPHIL - 1) {
            pthread_mutex_lock(&fork_mtx[right]);
            pthread_mutex_lock(&fork_mtx[left]);
        } else {
            pthread_mutex_lock(&fork_mtx[left]);
            pthread_mutex_lock(&fork_mtx[right]);
        }

        /* Eat. */
        local = spin_work(local);
        pthread_mutex_lock(&stats_mtx);
        meals[id]++;
        checksum += (uint64_t)id;
        work_acc += local & 0xff;
        pthread_mutex_unlock(&stats_mtx);

        pthread_mutex_unlock(&fork_mtx[left]);
        pthread_mutex_unlock(&fork_mtx[right]);
    }
    return (void *)local;
}

int
main(void)
{
    write(1, "QEMU-CPU-MODE-PHILO-READY\n", 26);

    for (int i = 0; i < NPHIL; i++)
        pthread_mutex_init(&fork_mtx[i], NULL);

    pthread_t th[NPHIL];
    for (long i = 0; i < NPHIL; i++)
        pthread_create(&th[i], NULL, philosopher, (void *)i);

    /* (1) Race-free snapshot point: the philosopher threads now exist, so
     * the contended dining region runs entirely under gem5 after restore. */
    snapshot_barrier();

    /* (2) The multicore region: wait for every philosopher to finish. */
    for (int i = 0; i < NPHIL; i++)
        pthread_join(th[i], NULL);

    /* (3) Validate the order-independent deterministic result. */
    uint64_t total = 0;
    int ok = 1;
    for (int i = 0; i < NPHIL; i++) {
        total += meals[i];
        if (meals[i] != ROUNDS)
            ok = 0;
    }
    if (total != EXPECT_TOTAL || checksum != EXPECT_CHECK)
        ok = 0;

    /* Report on the console - reaches gem5's terminal once PLIC/UART state
     * is restored and the UART interrupt path is live. */
    char msg[96];
    char *p = msg;
    const char *tag = ok ? "PHILO-DONE ok meals=" : "PHILO-DONE BAD meals=";
    while (*tag)
        *p++ = *tag++;
    p = u64_to_dec(p, total);
    const char *ck = " checksum=";
    while (*ck)
        *p++ = *ck++;
    p = u64_to_dec(p, checksum);
    *p++ = '\n';
    write(1, msg, (size_t)(p - msg));

    /* Wait for the console output to drain through the (interrupt-driven)
     * UART, then end the gem5 simulation cleanly. */
    tcdrain(1);
    m5_exit(0);
    return 0;
}
