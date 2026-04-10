# Stage 3c False-Sharing Methodology

## Goal

`rbook_test_false_sharing` creates repeated write-write contention on one
cache line shared between CPU 0 and CPU 15.

The benchmark is meant to make the CHI invalidation path visible in both the
cache statistics and the mesh link statistics.

## Placement Model

The test follows the repository's documented SE-mode placement rule.

- `main()` runs on CPU 0.
- Fifteen pthreads are created.
- In this configuration, one deterministic worker lands on CPU 15.
- All other workers exit before the measured stats window begins.

This avoids unsupported affinity syscalls while still producing a true
corner-to-corner coherence path.

## Shared-Line Layout

The benchmark uses a dedicated `mmap()` page and places the hot line at offset
`0x3c0`.

That offset is cache line 15 within the page, so in the Chapter 17 config it is
homed at `hnf15`.

Two adjacent `int` values on that line are updated independently:

- CPU 0 updates `shared_words[0]`
- CPU 15 updates `shared_words[1]`

The words are different C objects, but they occupy the same 64-byte line, so
the writes create false sharing.

## Measured Window

Before the stats window:

1. both shared words are initialized to zero
2. fifteen pthread workers are created
3. a `pthread_barrier_t` confirms that every worker has started and recorded its
   `getcpu()` result
4. the worker that landed on CPU 15 becomes the far-corner participant
5. all non-participant workers are joined
6. CPU 0 and CPU 15 each read the hot line once so both private caches hold it

The benchmark then does:

1. `m5_reset_stats(0, 0)`
2. release CPU 15 from a start flag on a separate cache line
3. CPU 0 and CPU 15 each run `128` iterations of `word += 1` by default
4. wait for CPU 15 to finish
5. `m5_dump_reset_stats(0, 0)`

Because the synchronization flags live on separate cache lines, the measured
window is dominated by the ownership transfers on the false-shared line.

The benchmark accepts an optional iteration count as `argv[1]`.
The `run` target passes that through `--options=...`, so you can scale the run
without recompiling:

```bash
make -C ruby-book/final/false_sharing run ITERATIONS=64
```

The run harness also launches `monitor_false_sharing.py`, which prints sparse
phase changes plus measured-window progress with elapsed time and ETA.
That monitor reads the console log from the host side, so the terminal stays
informative even when gem5 spends a long time in the coherence-heavy section.

## Confirmed SE-Mode pthread behavior

The companion probe binary `rbook_probe_pthreads.c` validates the assumptions
that Stage 3c relies on.

- `main()` starts on CPU 0
- 15 `pthread_create()` calls fill CPUs 1 through 15 in order
- `pthread_barrier_t` works under static RISC-V SE mode
- `pthread_join()` completes cleanly for every worker

That probe exists because plain `fork()` is the wrong primitive for this
benchmark.
Stage 3c needs one shared address space (`CLONE_VM`) so that both participants
touch the same false-shared cache line.

Two RISC-V SE-mode pitfalls matter here:

- `sched_setaffinity()` is not available, so thread placement must use creation
  order plus `getcpu()`
- pthread barriers and condition variables are futex-backed, so keep them
  outside the measured window if you want the stats block to reflect coherence
  traffic instead of suspension/wakeup behavior

The reported `CPU0_CYCLES` and `CPU15_CYCLES` values are therefore diagnostic
only.
`CPU0_CYCLES` includes the small cost of emitting sparse progress markers from
the main thread, so it is not meant to be compared directly against the worker
cycle count as a latency metric.

## Expected Signals

In the isolated stats block you should see all of the following:

- nonzero L1D demand activity on both `cpu0` and `cpu15`
- nonzero coherence counters such as `ReadUnique` and `CompAck`
- nonzero flits on the forward diagonal path from router 0 to router 15
- nonzero flits on the reverse diagonal path from router 15 back to router 0

The forward path is:

- `0 -> 1 -> 2 -> 3 -> 7 -> 11 -> 15`

The reverse path is the same links in the opposite direction.

## Typical Workflow

### Build

```bash
make -C ruby-book/final/false_sharing build
```

### Run

```bash
make -C ruby-book/final/false_sharing run
```

### Probe pthread placement

```bash
make -C ruby-book/final/false_sharing probe_run
```

### Check

```bash
make -C ruby-book/final/false_sharing check
```

### Report

```bash
make -C ruby-book/final/false_sharing report
```
