/*
 * rbook_probe_pthreads.c -- Minimal SE-mode pthread probe for Chapter 17.
 *
 * The probe validates the assumptions that Stage 3c depends on:
 * - main starts on CPU 0
 * - 15 pthread_create() calls fill CPUs 1..15 in order
 * - pthread_barrier_t works in static RISC-V SE mode
 * - pthread_join() completes cleanly for every worker
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define NUM_WORKERS 15
#define TOTAL_PARTICIPANTS 16

struct ProbeState
{
    pthread_barrier_t barrier;
    int worker_cpu[NUM_WORKERS];
    int worker_seen[NUM_WORKERS];
    int cpu15_hits;
};

struct WorkerArg
{
    struct ProbeState *state;
    int worker_index;
};

static inline int
gem5_cpu_id(void)
{
    unsigned cpu = 0;
    unsigned node = 0;
    long ret = syscall(SYS_getcpu, &cpu, &node, 0);

    return ret == 0 ? (int)cpu : -1;
}

static void
fail(const char *message)
{
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
}

static void
log_phase(const char *phase)
{
    printf("RBOOK_PTHREAD_PHASE %s\n", phase);
    fflush(stdout);
}

static void *
worker_main(void *arg)
{
    struct WorkerArg *worker = arg;
    struct ProbeState *state = worker->state;
    int cpu = gem5_cpu_id();
    int rc;

    if (cpu < 0) {
        fail("worker getcpu failed");
    }

    state->worker_cpu[worker->worker_index] = cpu;
    __atomic_store_n(&state->worker_seen[worker->worker_index], 1,
                     __ATOMIC_RELEASE);

    rc = pthread_barrier_wait(&state->barrier);
    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
        fail("worker barrier wait failed");
    }

    if (cpu == 15) {
        __atomic_fetch_add(&state->cpu15_hits, 1, __ATOMIC_SEQ_CST);
    }

    return NULL;
}

int
main(void)
{
    pthread_t threads[NUM_WORKERS];
    struct WorkerArg worker_args[NUM_WORKERS];
    struct ProbeState state;
    int main_cpu;
    int rc;

    memset(&state, 0, sizeof(state));

    main_cpu = gem5_cpu_id();
    if (main_cpu != 0) {
        fprintf(stderr, "FAIL: expected main on CPU 0, got %d\n", main_cpu);
        return 1;
    }

    rc = pthread_barrier_init(&state.barrier, NULL, TOTAL_PARTICIPANTS);
    if (rc != 0) {
        fprintf(stderr, "FAIL: pthread_barrier_init: %s\n", strerror(rc));
        return 1;
    }

    log_phase("START");

    for (int i = 0; i < NUM_WORKERS; ++i) {
        worker_args[i].state = &state;
        worker_args[i].worker_index = i;
        rc = pthread_create(&threads[i], NULL, worker_main, &worker_args[i]);
        if (rc != 0) {
            fprintf(stderr, "FAIL: pthread_create[%d]: %s\n", i, strerror(rc));
            return 1;
        }
    }

    log_phase("THREADS_LAUNCHED");

    rc = pthread_barrier_wait(&state.barrier);
    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
        fail("main barrier wait failed");
    }

    log_phase("BARRIER_DONE");

    for (int i = 0; i < NUM_WORKERS; ++i) {
        pthread_join(threads[i], NULL);
    }

    log_phase("JOINS_DONE");

    printf("RBOOK_PTHREAD MAIN_CPU %d\n", main_cpu);
    for (int i = 0; i < NUM_WORKERS; ++i) {
        printf("RBOOK_PTHREAD WORKER_%02d_CPU %d\n", i + 1,
               state.worker_cpu[i]);
    }
    printf("RBOOK_PTHREAD CPU15_HITS %d\n", state.cpu15_hits);

    for (int i = 0; i < NUM_WORKERS; ++i) {
        int expected_cpu = i + 1;

        if (!state.worker_seen[i]) {
            fprintf(stderr, "FAIL: worker %d never reported CPU\n", i + 1);
            return 1;
        }
        if (state.worker_cpu[i] != expected_cpu) {
            fprintf(stderr, "FAIL: worker %d expected CPU %d, got %d\n", i + 1,
                    expected_cpu, state.worker_cpu[i]);
            return 1;
        }
    }

    if (state.cpu15_hits != 1) {
        fprintf(stderr, "FAIL: expected exactly one cpu15 hit, got %d\n",
                state.cpu15_hits);
        return 1;
    }

    printf("PASS\n");
    fflush(stdout);
    pthread_barrier_destroy(&state.barrier);
    return 0;
}
