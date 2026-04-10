# Stage 3c False-Sharing Methodology

## Goal

`rbook_test_false_sharing` measures the extra round-trip latency caused by
false sharing between CPU 0 and CPU 15.

It does that by comparing two ping-pong phases that use the same turn-taking
protocol:

- a control ping-pong with only separate control/data lines
- a false-sharing ping-pong where both cores also write adjacent `int` values
  in the same 64-byte cache line

The difference between those two averages is the cleanest latency estimate for
the ownership-bounce cost on the false-shared line.

## Placement Model

The benchmark follows the repository's documented SE-mode placement rule.

- `main()` runs on CPU 0
- fifteen pthreads are created
- one deterministic worker lands on CPU 15
- all other workers exit before the measured phases begin

No affinity syscalls are needed.
Placement comes from creation order plus `getcpu()`.

The companion probe binary `rbook_probe_pthreads.c` validates that the Chapter
17 setup really behaves this way under static RISC-V SE mode:

- `main()` starts on CPU 0
- workers fill CPUs 1 through 15 in order
- `pthread_barrier_t` works
- `pthread_join()` completes cleanly

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

## Measurement Model

The benchmark has two measured windows.

### Control ping-pong

CPU 0 and CPU 15 exchange a token using `req_flag` and `ack_flag`, both placed
on separate cache lines.
Each side also increments a private word on its own cache line.

For each round:

1. CPU 0 increments its private word
2. CPU 0 stores `req_flag = round`
3. CPU 15 waits for that round number
4. CPU 15 increments its private word
5. CPU 15 stores `ack_flag = round`
6. CPU 0 waits for the matching acknowledgement and records the round-trip
   `rdcycle` delta

This phase measures the control-plane cost of turn-taking across the mesh
without false sharing on the data line.

### False-sharing ping-pong

The turn-taking protocol stays the same, but each side now increments its word
inside the shared hot line.

For each round:

1. CPU 0 increments `shared_words[0]`
2. CPU 0 stores `req_flag = round`
3. CPU 15 waits for that round number
4. CPU 15 increments `shared_words[1]`
5. CPU 15 stores `ack_flag = round`
6. CPU 0 waits for the acknowledgement and records the round-trip `rdcycle`
   delta

Because both data updates hit the same cache line, every round forces the line
to change ownership in both directions.

## Why This Produces A Cleaner Latency Estimate

The earlier free-running version measured contention throughput, not a clean
per-handoff latency.
One core could get ahead and perform several writes while it still owned the
line.

The ping-pong design fixes that.
Each round has one explicit handoff from CPU 0 to CPU 15 and one return
handoff.
Subtracting the control average from the false-sharing average removes most of
the cost of the turn-taking protocol itself and leaves the extra coherence cost
of the shared line bounce.

## Stats Windows

The benchmark wraps both phases with isolated stats windows:

1. control ping-pong window
2. false-sharing ping-pong window

The checker compares those two windows instead of expecting the control case to
be zero-traffic.
The control window already includes mesh traffic from the turn-taking flags.

The strongest expected signals are:

- false-sharing average latency is larger than control average latency
- `CompAck` / `SendCompAck` totals are larger in the false-sharing window
- total diagonal-path flits are larger in the false-sharing window
- both windows show activity on CPU 0 and CPU 15

`ReadUnique` is still reported, but it is not used as a strict pass/fail gate.
In practice it can vary depending on the exact ownership state reached at the
start of the window.

## Runtime Visibility

The run harness launches `monitor_false_sharing.py`, which prints sparse phase
changes plus a low-noise heartbeat with elapsed time.

The benchmark intentionally does **not** print per-iteration markers inside the
measured loop.
That keeps the final latency estimate clean.

## RISC-V SE-Mode Pitfalls

Two pitfalls matter here:

- `sched_setaffinity()` is not available in RISC-V SE mode, so use creation
  order plus `getcpu()`
- pthread synchronization is futex-backed, so keep barriers outside the
  measured region if you want the measured window to reflect coherence and mesh
  traffic instead of suspension/wakeup effects

## Typical Workflow

### Build

```bash
make -C ruby-book/final/false_sharing build
```

### Probe pthread placement

```bash
make -C ruby-book/final/false_sharing probe_run
```

### Run

```bash
make -C ruby-book/final/false_sharing run
```

The iteration count is runtime-configurable:

```bash
make -C ruby-book/final/false_sharing run ITERATIONS=64
```

### Check

```bash
make -C ruby-book/final/false_sharing check
```

### Report

```bash
make -C ruby-book/final/false_sharing report
```
