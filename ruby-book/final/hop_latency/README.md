# Stage 3b Hop-Latency Methodology

## Goal

`rbook_test_hop_latency` measures how much extra latency core 0 pays when a
load is homed at a far HN-F slice instead of a near one.

The test is designed to isolate hop distance inside the CHI + Garnet mesh.

## What The Test Measures

The benchmark compares two loads:

- a near load that homes at `hnf0`
- a far load that homes at `hnf15`

Both loads are intended to miss in the private caches and hit in the LLC.

That means the measured delta should mainly reflect the extra network distance,
not a DRAM access.

## Address Selection

HN-F selection comes from physical address bits `[9:6]` in the Chapter 17 CHI
configuration.

Because a page offset is preserved across translation in SE mode, one
page-aligned target page is enough to choose exact HN-F homes.

The benchmark uses:

- near line: offset `0x000` -> `hnf0`
- far line: offset `0x3c0` -> `hnf15`

This is why the test uses a dedicated 4 KiB `mmap()` region rather than an
arbitrary `malloc()` buffer.

## Why There Is A 4 MiB Eviction Sweep

The benchmark must evict the private caches before each timed load.

The Chapter 17 run uses default cache sizes from `configs/common/Options.py`:

- `L1D = 64KiB`
- `L2 = 2MiB`
- `L3 = 16MiB`

The test therefore sweeps a separate 4 MiB buffer before each measurement.

That is large enough to flush the private `L1/L2` caches, but still small
enough that the target lines should remain resident in the distributed LLC
slices.

## Timed Measurement Sequence

For each target class, the benchmark does this:

1. Touch both target lines once so they are resident in their home LLC slices.
2. Sweep the 4 MiB eviction buffer to flush private caches.
3. Read `rdcycle`.
4. Load the target byte.
5. Read `rdcycle` again.
6. Record the cycle difference.

The benchmark repeats that sequence for both the near and far addresses and
prints sample vectors plus averages.

## Why The Test Also Dumps Two Isolated Stats Windows

The timed loop alone is enough to measure latency, but not enough to prove the
actual path through the mesh.

Whole-run `stats.txt` contains traffic from:

- the eviction sweep
- page faults and startup activity
- the benchmark's own instruction and stack traffic

To make the topology evidence readable, the benchmark performs two additional
single-load probes:

1. near isolated probe
2. far isolated probe

Each probe is wrapped with:

- `m5_reset_stats(0, 0)`
- one demand load
- `m5_dump_reset_stats(0, 0)`

The checker and report scripts read those first two dumped stat blocks as the
near-only and far-only windows.

## How To Interpret The Stats

The strongest expected signals are:

- near block:
  - demand access at `hnf0`
  - no demand access at `hnf15`
  - no traffic through the deeper diagonal path links
- far block:
  - demand access at `hnf15`
  - nonzero traffic on the XY path from router 0 to router 15

For this topology, the expected far request path is:

- `int_links00`
- `int_links01`
- `int_links02`
- `int_links33`
- `int_links34`
- `int_links35`

## Expected Noise

The isolated stats windows are not literally a single architectural load and
nothing else.

While stats are enabled, the benchmark still executes instructions and may
touch stack lines or nearby code/data lines.

That can create small unrelated LLC accesses or a tiny amount of traffic on the
first local mesh hop.

The analysis scripts therefore treat deep far-path activation as the key proof,
instead of requiring every unrelated counter to be exactly zero.

## Typical Workflow

### Build

```bash
make -C ruby-book/final/hop_latency rbook_test_hop_latency
```

Or use the explicit build target:

```bash
make -C ruby-book/final/hop_latency build
```

### Run

```bash
outdir="m5out/rbook-hop-latency-$(date +%Y%m%d-%H%M%S)"
mkdir "$outdir"
./build/RISCV/gem5.opt -d "$outdir" \
    configs/example/rbook_mesh_config.py \
    --cmd=ruby-book/final/hop_latency/rbook_test_hop_latency \
    > "$outdir/console.log" 2>&1
```

### Check

```bash
python3 ruby-book/final/hop_latency/check_hop_latency.py \
    "$outdir" \
    "$outdir/console.log"
```

### Generate A Markdown Summary Report

```bash
python3 ruby-book/final/hop_latency/report_hop_latency.py \
    "$outdir" \
    "$outdir/console.log" \
    "ruby-book/final/hop_latency/ValidatedRunReport.md"
```

## Makefile Workflow Targets

The dedicated Makefile can drive the full Stage 3b flow.

### Full workflow

```bash
make -C ruby-book/final/hop_latency
```

This default target does all three steps:

1. compile `rbook_test_hop_latency`
2. run gem5 and save the latest output directory into `.last_outdir`
3. run the checker and generate `ValidatedRunReport.md`

### Individual targets

```bash
make -C ruby-book/final/hop_latency build
make -C ruby-book/final/hop_latency run
make -C ruby-book/final/hop_latency check
make -C ruby-book/final/hop_latency report
```

There is also a top-level wrapper target:

```bash
make -C ruby-book/final hop_latency_report
```

## What Counts As A Good Result

A healthy Stage 3b run should show all of the following:

- `far_avg > near_avg`
- a clearly positive delta, ideally close to the chapter's expected value
- near isolated demand traffic centered on `hnf0`
- far isolated demand traffic centered on `hnf15`
- far isolated mesh traffic on the expected XY path links

If the delta collapses toward zero, the private-cache eviction sweep is likely
too small.

If the delta becomes much larger than expected, the sweep may be evicting the
target from LLC and turning the measurement into a memory-service test instead
of a hop-distance test.
