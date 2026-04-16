# Stage 3f Hotspot / Backpressure Methodology

## Goal

`rbook_test_hotspot` creates many-to-one traffic toward `HNF15` in the Chapter
17 4x4 CHI mesh.

The benchmark is meant to answer two questions:

- how much hotspot traffic can one core inject
- how the destination-side network state changes as more cores target the same
  home node

The current Chapter 17 mesh config now defaults to `RiscvO3CPU`, so this test
also serves as the working single-core throughput probe for the O3 setup.

## Placement Model

The benchmark uses the same SE-mode placement rule as the other Chapter 17
tests.

- `main()` starts on CPU 0
- fifteen pthreads are created
- workers land on CPUs 1 through 15 in order
- the benchmark fails fast if that mapping does not hold

No affinity syscalls are required.
The test relies on static RISC-V SE scheduling plus `getcpu()` validation.

## Why The Access Pattern Targets HNF15

In this CHI configuration, the HN-F is chosen from address bits `[9:6]`.
Within a 4 KiB page, those bits repeat every `16 * 64 = 1024` bytes.

That means one page contains four different cache lines that all home at the
same HN-F.
For `HNF15`, the relevant offsets are:

- `0x3c0`
- `0x7c0`
- `0xbc0`
- `0xfc0`

The benchmark touches exactly those four lines in every page.

## Why The Test Uses Four HNF15 Lines Per Page

The earlier version touched only one `HNF15` line per page.
That worked, but it left three valid `HNF15` lines in the same page unused.

The current version touches all four.
That improves single-core injection for the same page footprint because:

- the TLB and page walk overhead is amortized over more targeted lines
- one O3 core sees more independent HNF15-bound misses per page
- the test increases pressure on the shared destination without changing the
  home-node mapping rule

## Measured Working Set

By default, each active CPU uses:

- `2048` pages
- `8 MiB` of mapped space
- `4` HNF15 lines per page
- `8192` targeted line accesses per measured window for one active CPU

The page footprint is much larger than the private `L2`, so the core cannot
simply recycle a tiny hot set in the private hierarchy.

The checker confirms that the traffic really reaches the target HN-F by testing
that `HNF15` demand accesses stay very close to one per requested line.

## Load Generation Model

Each active CPU owns a private anonymous mapping.
There is no false sharing between CPUs.

Inside the hot loop, each CPU walks its private pages and loads the four
`HNF15`-homed lines from each page.
The loop uses multiple independent accumulator lanes so one O3 core can keep
more misses in flight instead of building a single long load-dependency chain.

That makes the signal cleaner:

- the traffic is still read-only and private
- every targeted line still homes at `HNF15`
- increased traffic reflects higher injection, not coherence bouncing

## Measured Windows

The benchmark sweeps five offered-load points:

- `1` active CPU
- `2` active CPUs
- `4` active CPUs
- `8` active CPUs
- `16` active CPUs

For each point:

1. all threads synchronize outside the measured region
2. the benchmark sets the active CPU count
3. `m5_reset_stats(0, 0)` starts a clean stats window
4. active CPUs run the hotspot stream once
5. `m5_dump_reset_stats(0, 0)` appends one isolated stats block to
   `stats.txt`

So one gem5 run still creates one `stats.txt`, but that file contains one stats
block per measured window plus the final end-of-run dump.

## What The Checker Treats As Strong Evidence

The strongest signals are:

- `HNF15` demand accesses stay near one per requested line
- `HNF15` ext-link traffic rises strongly with offered load
- router 15 buffer activity rises strongly with offered load
- destination-side flit cost per requested line rises with offered load
- per-thread throughput efficiency falls as more CPUs target the same HN-F

`average_flit_queueing_latency` is still reported, but it is not the only pass
criterion.
In practice, it can move non-monotonically even when destination pressure is
clearly increasing.

## Interpreting Backpressure

This test is best interpreted as a hotspot backpressure / congestion test for
the destination region around `HNF15`.

It proves that increasing offered load creates more queueing and more
destination-side work in the `HNF15` path.
It does **not** by itself isolate whether the dominant bottleneck is the NI,
the router input buffers, or both.

## Shared Links vs Per-Vnet Links

The Chapter 17 mesh now supports Garnet's `--per-vnet-links` option through
`CustomMesh.py`.
When the option is off, each adjacent router pair shares one physical mesh link
per direction across all CHI virtual networks.
When the option is on, the mesh creates one physical link per vnet per
direction.

For this CHI configuration, `number_of_virtual_networks=4`, so enabling
per-vnet links grows the mesh-internal link count from `48` to `192` while
leaving the RN-F bridge links unchanged.

The topology change was verified in two ways:

- shared-links run: `48` mesh internal links, typically carrying mixed
  `vnet-0/1/2/3` traffic on the same physical link
- per-vnet run: `192` mesh internal links, with each active mesh link carrying
  exactly one vnet

## Measured Per-Vnet-Link Impact

Two full `1/2/4/8/16` sweeps were run with the same benchmark binary and page
count:

- shared links: `m5out/rbook-hotspot-20260416-185425`
- per-vnet links: `m5out/rbook-hotspot-20260416-185723`

Both runs passed the hotspot checker.

### Throughput delta

| Threads | Shared `ops/cycle` | Per-vnet `ops/cycle` | Delta |
| ---: | ---: | ---: | ---: |
| 1 | 0.055099 | 0.055185 | +0.16% |
| 2 | 0.059744 | 0.060207 | +0.78% |
| 4 | 0.065590 | 0.066042 | +0.69% |
| 8 | 0.068339 | 0.068581 | +0.35% |
| 16 | 0.073801 | 0.074177 | +0.51% |

### Network latency delta

| Threads | Queueing Shared | Queueing Per-vnet | Delta | Network Shared | Network Per-vnet | Delta |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 2546.45 | 2475.25 | -2.80% | 16960.97 | 16954.64 | -0.04% |
| 2 | 1835.61 | 1808.92 | -1.45% | 19888.81 | 19730.51 | -0.80% |
| 4 | 1391.67 | 1339.90 | -3.72% | 18138.19 | 18035.48 | -0.57% |
| 8 | 1242.07 | 1173.87 | -5.49% | 17152.21 | 17055.20 | -0.57% |
| 16 | 2210.84 | 2132.05 | -3.56% | 14663.71 | 14573.98 | -0.61% |

### Destination-side interpretation

At `16` threads:

- `HNF15` demand accesses are unchanged: `131212` shared vs `131204` per-vnet
- `HNF15` ext-link flits per op are unchanged: `15.5147` shared vs `15.5198`
- router 15 buffer reads per op are unchanged: `17.2271` shared vs `17.2313`
- `system.ruby.hnf15.cntrl.rspOut.m_buf_msgs` drops from `6.566334` to
  `6.356519`
- `system.ruby.hnf15.cntrl.reqRdy.m_stall_count` drops from `104` to `97`

The practical result is:

- per-vnet links reduce mixed-vnet link contention in the mesh
- queueing latency improves consistently
- throughput improves only modestly, by about `0.2%` to `0.8%`
- the dominant bottleneck for this test still sits near the destination side of
  the `HNF15` path rather than in shared mesh-link bandwidth alone

## Typical Workflow

### Build

```bash
make -C ruby-book/final/hotspot build
```

### Run and check

```bash
make -C ruby-book/final/hotspot report
```

### Run with explicit shared mesh links

```bash
make -C ruby-book/final/hotspot run_shared_links
```

### Run with per-vnet dedicated mesh links

```bash
make -C ruby-book/final/hotspot run_per_vnet_links
```

### Change page count

```bash
make -C ruby-book/final/hotspot report PAGES=4096
```

### Key artifacts

- benchmark: `ruby-book/final/hotspot/rbook_test_hotspot.c`
- checker: `ruby-book/final/hotspot/check_hotspot.py`
- markdown summary: `ruby-book/final/hotspot/ValidatedRunReport.md`
- mesh config: `configs/example/rbook_mesh_config.py`
- topology implementation: `configs/topologies/CustomMesh.py`
