# Link Pressure Benchmark

Companion to `ruby-book/final/hotspot` and `ruby-book/final/hotspot-heavy`.

## Goal

Construct a workload that loads the **4x4 CHI mesh physical links** as hard as
possible, so that switching to Garnet's `--per-vnet-links` option produces a
clearly measurable throughput gain.

The prior `hotspot` study showed per-vnet-links giving only `~0.2%` to `~0.8%`
improvement; `hotspot-heavy` saw no improvement at all
(`−0.02%`, within noise). Both concentrate demand on **one** home node (HNF15),
so the binding constraint is the HNF service rate, not the mesh links.

`link-pressure` is the complementary case: spread demand uniformly across all
16 HNFs so that (a) no single HNF serializes traffic, (b) the mesh links are
the constraint, and (c) multiple CHI vnets compete for the same physical link
per direction — exactly the scenario `--per-vnet-links` is designed to relieve.

## Method

- **Shared buffer** of `STREAM_PAGES * 4 KiB` (default `1024` pages =
  `4 MiB`) is allocated once by CPU 0 and pre-warmed line-by-line so every
  cache line is resident in the 16 MiB LLC before measurement.
- `1, 2, 4, 8, 16` active threads sweep (same structure as `hotspot/`).
- Each thread streams the buffer cache-line-sequentially starting at offset
  `thread_id * (total_lines / 16)` so the 16 workers hit **16 different
  HNFs** at the barrier release and diverge afterwards.
- HNF interleaving is on `addr[9:6]`, so sequential streaming walks HNF 0 →
  1 → ... → 15 → 0 → 1 → ... . Over a full buffer pass each HNF sees
  `total_lines / 16` demand accesses.
- Buffer size `>=` L2 (`2 MiB`) so streaming constantly misses L2 and
  generates LLC traffic; buffer size `<` L3 (`16 MiB`) so after warmup the
  LLC holds every line and the measurement is mesh-link-bound rather than
  DRAM-bound.

## Build and run

```bash
# Build the benchmark binary (riscv64-linux-gnu-gcc required)
make -C ruby-book/final/link-pressure build

# Run with shared physical links (default)
make -C ruby-book/final/link-pressure run_shared_links

# Run with --per-vnet-links enabled
SIM_TIMEOUT=15m make -C ruby-book/final/link-pressure run_per_vnet_links

# Run both and put reports in ValidatedRunReport.md
SIM_TIMEOUT=15m make -C ruby-book/final/link-pressure compare
```

Per-vnet runs take noticeably longer in host wall time because the mesh has
~3x more physical links to simulate; use `SIM_TIMEOUT=15m` or higher.

## Variants (Makefile knobs)

| Knob | Default | Effect |
|---|---|---|
| `PAGES` | `1024` | Shared buffer size in 4 KiB pages |
| `MEM_SIZE` | `512MiB` | gem5 `--mem-size` |
| `PER_VNET_LINKS` | unset | Set to `1` to pass `--per-vnet-links` |
| `RUN_TAG` | `rbook-link-pressure` | Prefix for the m5out directory name |

## Output

Each `make run` writes to `$REPO_ROOT/m5out/<RUN_TAG>-<datetime>/`. The
`console.log` carries per-window throughput; `stats.txt` carries network and
HNF counters. `make check` runs `check_link_pressure.py` and `make report`
also writes a Markdown summary to `ValidatedRunReport.md`.

## Key metric

The headline metric is **16-thread line ops/cycle** (throughput under full
mesh pressure). Secondary: average flit queueing latency at 16 threads,
which should drop when the binding link contention is relieved.

---

## Investigation history

Each iteration version tag points to the m5out directories and key delta
against the previous iteration. Numbers are from the first dumped stats
block of the corresponding `make run`.

### v1a — initial shape, 1 MiB shared buffer (fails)

`2026-04-16`, pages=`256` (buffer `1 MiB`)

| Threads | Shared ops/cy | Shared HNF-min | Notes |
|---:|---:|---:|---|
| 1 | 0.142 | **0** | 1 MiB fits entirely in CPU 0's 2 MiB L2 after warmup, so single-thread window generates **zero HNF traffic** |
| 2 | 0.090 | 1024 | CPU 0 still hits L2; CPU 1 slogs through fresh cache fills; total throughput worse than 1t |
| 4 | 0.396 | 2048 | recovers once multiple cold caches pump |
| 8 | 0.785 | 4096 | |
| 16 | 1.230 | 8206 | |

Learned: buffer **must exceed L2 size (2 MiB)** or the single-thread window
has no LLC traffic, and a post-warmup CPU sees nothing to miss.

Fix: increase default buffer to `4 MiB` (`1024` pages). This guarantees
buffer overflows private L2 at any thread count.

### v1b — 4 MiB buffer, per-vnet-links wins (success)

`2026-04-16`, pages=`1024` (buffer `4 MiB`)

- shared: `m5out/rbook-link-pressure-v1b-shared-20260416-225348`
- per-vnet: `m5out/rbook-link-pressure-v1b-pervnet-20260416-232515`

#### Throughput

| Threads | Shared ops/cy | Per-vnet ops/cy | Delta |
|---:|---:|---:|---:|
| 1 | 0.07299 | 0.07296 | −0.04% |
| 2 | 0.18455 | 0.18535 | +0.43% |
| 4 | 0.36364 | 0.36688 | +0.89% |
| 8 | 0.59262 | 0.60907 | +2.78% |
| 16 | 0.97281 | **1.10306** | **+13.39%** |

#### Queueing latency (lower is better)

| Threads | Shared | Per-vnet | Delta |
|---:|---:|---:|---:|
| 16 | 31342.82 | 26531.56 | **−15.35%** |

#### Network latency (lower is better)

| Threads | Shared | Per-vnet | Delta |
|---:|---:|---:|---:|
| 16 | 16339.07 | 15208.52 | −6.92% |

#### HNF uniformity (16-thread window)

Spread `max/min = 1.002` on both runs — load is practically uniform
across all 16 HNFs, confirming the workload is mesh-link-bound, not
HNF-bound.

#### Vnet composition (shared-links 16-thread window)

| Vnet | Flits received | Share |
|---|---:|---:|
| 0 (REQ) | 3,930,764 | 19.20% |
| 1 (RSP) | 1,474 | 0.01% |
| 2 (SNP) | 3,932,765 | 19.21% |
| 3 (DAT) | 12,603,784 | **61.58%** |
| total | 20,468,787 | 100% |

DAT dominates. On a shared physical link DAT packets (5 flits for a
64 B line on a 128-bit link: 1 header + 4 data) head-of-line-block
single-flit REQ and SNP. `--per-vnet-links` splits each adjacent-router
link into 4 independent physical links (one per vnet), so REQ and SNP
no longer queue behind DAT bursts — exactly the +13.4% at 16 threads.

### v2 — 8 MiB buffer (plateau check)

`2026-04-17`, pages=`2048` (buffer `8 MiB`, exactly half of the 16 MiB LLC)

- shared: `m5out/rbook-link-pressure-v2-shared-20260416-234114`
- per-vnet: `m5out/rbook-link-pressure-v2-pervnet-20260416-234330`

#### Throughput

| Threads | Shared ops/cy | Per-vnet ops/cy | Delta |
|---:|---:|---:|---:|
| 1 | 0.08196 | 0.08216 | +0.24% |
| 2 | 0.18410 | 0.18509 | +0.54% |
| 4 | 0.36509 | 0.36811 | +0.83% |
| 8 | 0.59311 | 0.60406 | +1.85% |
| 16 | 0.97035 | **1.09858** | **+13.22%** |

#### Queueing latency (16 threads)

| Shared | Per-vnet | Delta |
|---:|---:|---:|
| 31237.43 | 25935.26 | −16.97% |

Doubling the buffer from 4 MiB to 8 MiB changes nothing meaningful at 16
threads: the per-vnet gain lands at `+13.22%` vs v1b's `+13.39%`, and the
queueing-latency reduction is the same `~−15–17%`. The 1-thread window
did get ~12% faster (0.0730 → 0.0820 ops/cy) because the larger buffer
forces a higher fraction of L2 misses, producing more steady-state mesh
traffic per thread — but that does not move the 16-thread saturation
plateau, which is already link-bound at 4 MiB.

Conclusion: **4 MiB (`PAGES=1024`) is kept as the default** because it
hits the plateau at lower simulation wall time. Use `PAGES=2048` only
when wanting to cross-check that the plateau is real rather than an
artifact of a too-small buffer.

### Current bottleneck (16 threads, per-vnet-links)

Headline: **DAT-vnet physical links into the top-row and bottom-row
edge HNF tiles** are the binding resource. Per-core MLP is pinned by
the resulting round-trip, not by any CPU/cache queue cap.

#### Average flit latency, broken down per vnet

From the v1b 16-thread per-vnet stats block (500 ticks = 1 CPU cycle
@ 2 GHz). `Network` = transit time through routers/links; `Source NI
queueing` = time a flit waits in its injecting controller's output
buffer before entering the network.

| Vnet | Network (ticks) | Source NI queueing (ticks) | Total (ticks) | CPU cycles |
|---|---:|---:|---:|---:|
| 0 REQ (RN → HN) | 9,823 | 2,015 | 11,838 | 23.7 |
| 1 RSP | 4,050 | 570 | 4,620 | 9.2 |
| 2 SNP | 9,230 | 863 | 10,093 | 20.2 |
| **3 DAT (HN → RN)** | **18,788** | **42,337** | **61,125** | **122.2** |
| all (weighted) | 15,209 | 26,532 | 41,740 | 83.5 |

The aggregate `qlat = 26,532` hides that **~86% of all mesh queueing
is on the DAT vnet alone**. REQ and SNP flits spend ~20% of their
total time queueing; DAT flits spend 69% of their total time queueing
(42k of 61k ticks).

gem5 in this config does not emit a max/peak flit latency; the per-vnet
totals above are the proxy. DAT's queueing/transit ratio is `2.25x` vs
REQ `0.21x`.

#### Where on the mesh the DAT queue physically sits

DAT flits queue at the **source-side network interface** of each HNF
(stat: `system.ruby.hnfN.cntrl.datOut.m_buf_msgs` — average number of
messages waiting in the HNF's DAT output buffer):

| HNF | Router position (row, col) | datOut avg msgs waiting |
|---:|---|---:|
| **1** | top edge (0, 1) | **81.3** |
| **14** | bottom edge (3, 2) | **73.9** |
| **2** | top edge (0, 2) | **57.2** |
| **13** | bottom edge (3, 1) | **47.6** |
| 5 | interior (1, 1) | 19.8 |
| 6 | interior (1, 2) | 16.8 |
| 9 | interior (2, 1) | 15.5 |
| 10 | interior (2, 2) | 12.8 |
| 0, 3, 12, 15 | corners | 6.5 – 7.9 |
| 4, 7, 8, 11 | side edges (col 0 or col 3, not a corner) | 3.2 – 4.1 |

Pile-up is concentrated on **four edge-row HNFs**, with totals 20–80
messages waiting. Corners and side edges have near-empty queues.

#### Crossbar activity per mesh router (flits/cycle, window 4)

```
       col 0   col 1   col 2   col 3
row 0: 1.69    2.40    2.38    1.69
row 1: 2.35    3.03    3.03    2.35   <- interior routers near 3 flits/cy
row 2: 2.27    2.91    2.91    2.27
row 3: 1.61    2.26    2.27    1.61
```

Interior routers 5, 6, 9, 10 push the most total flits/cy (~3.0) but
their local HNFs' DAT-output queues stay small. Top/bottom edge
routers (1, 2, 13, 14) push less total traffic but their local HNFs'
DAT queues explode. The reason is the DIRECTION of outflow, not the
total volume.

#### Why top/bottom-edge HNFs pile up where interior HNFs do not

- Interior routers (5, 6, 9, 10) have **4 mesh outputs** (N, S, E, W).
  Their local HNF's outbound DAT is dispersed across all four.
- Top-row routers (1, 2) have **3 mesh outputs** (S, E, W — no N).
  Bottom-row routers (13, 14) have **3 mesh outputs** (N, E, W — no S).
- With XY dimension-ordered routing and uniform destination spread, an
  edge HNF like HNF1 sends DAT east-bound to 8 of 16 CPUs (col 2 and
  col 3 on all four rows), west-bound to 4 CPUs (col 0 on all rows),
  south-bound to 3 CPUs (col 1, rows 1–3), and keeps 1 local. The
  **east-facing link `r1 → r2` carries 50% of HNF1's DAT output**
  on top of the row-0 transit traffic from HNF0 going east.
- That single east-facing top-row link at `1 flit/cycle/vnet` is the
  chokepoint. The NI cannot drain the HNF's DAT output fast enough,
  so `datOut.m_buf_msgs` climbs and the DAT flits accumulate
  source-side queueing latency (the 42k ticks in the per-vnet table).
- Side-edge HNFs (4, 7, 8, 11) have the opposite geometry (3 mesh
  outputs: N, S, plus one of E/W). They route ~half their DAT
  north/south where transit is lighter, so their queues stay short.

#### What is NOT the bottleneck

| Resource | Cap | Avg size | Avg utilization |
|---|---:|---:|---:|
| L1D TBEs (per CPU) | 32 | 23.5–24.1 | 0.74 |
| L2 TBEs (per CPU) | 64 | ~23.3 | 0.36 |
| HNF TBEs (per HNF) | 64 | 12.0–16.7 | 0.19–0.26 |
| Sequencer max outstanding | 32 | ~24 | 0.74 |

The 74% L1D TBE utilization looks binding but isn't: v3 (below)
widens the cap to 48 and 64 and the average occupancy stays at
~25, confirming the per-core in-flight count is pinned by
`T_round-trip`, not by the cap.

#### Little's-Law framing

$$ N_{\text{in-flight}} = R_{\text{miss}} \cdot T_{\text{round-trip}} $$

With `N ≈ 24` per core and `T_round-trip ≈ 83` CPU cycles, per-core
miss rate `R ≈ 24/83 ≈ 0.29` misses/cycle, and total system miss
rate `≈ 16 * 0.29 ≈ 4.6` misses/cycle. Measured throughput of
`1.10` line ops/cycle × ~4x average flits per line op matches this
— the mesh is the ceiling. Raising `N` (widening L1D TBEs) only
raises `T` (more queueing on the already-saturated DAT edge links),
so `R` stays flat or falls.

#### Real levers (not measured here)

1. **Wider physical links** (`--link-width-bits` 128 → 256). Halves
   the flit count of a 64 B DAT packet (5 → 3 flits), which directly
   relieves the top-row east-bound DAT link.
2. **Non-XY routing** (adaptive or YX-biased for DAT vnet) so
   edge-HNF DAT can go Y-first and skip the loaded row-0 / row-3
   east-west links.
3. **Relocate HNFs 1, 2, 13, 14** off the top/bottom edge rows
   (e.g., move LLC slices to interior tiles only, or add skipping
   links between non-adjacent interior routers).
4. **Add a second physical link per vnet in the direction of the
   dominant DAT flow** (east along row 0 and row 3).

`--per-vnet-links` is already applied; it bought `+13.4%` by removing
DAT→REQ HoL blocking on the shared 128b physical link, but it does
not widen the per-vnet capacity itself — that takes `--link-width-bits`
or topology changes.

### v3 — L1D TBE widening sweep (confirms mesh is the ceiling)

`2026-04-17`, pages=`1024` (4 MiB buffer), per-vnet-links enabled.

- v1b baseline: L1D TBE=32, seq=32 → `1.10306` ops/cy
- v3 TBE=48: `m5out/rbook-link-pressure-v3-tbe48-20260417-003146`
- v3 TBE=64: `m5out/rbook-link-pressure-v3-tbe64-20260417-003740`

| L1D TBE / seq | 16t ops/cy | Δ vs v1b | 16t qlat (ticks) | L1D avg occupancy |
|---:|---:|---:|---:|---:|
| **32 / 32** (v1b) | **1.10306** | — | 26,532 | 23.7 / 32 (74%) |
| 48 / 48 | 1.07245 | **−2.78%** | 30,580 | 25.5 / 48 (53%) |
| 64 / 64 | 1.09145 | **−1.05%** | 30,873 | 25.6 / 64 (40%) |

Key signal: average L1D TBE occupancy grew only from `23.7` to `25.5`
to `25.6` across 32/48/64 slots — essentially flat. If the cap had
been binding, doubling it would fill the new slots. Instead the mesh
just soaks up more latency (`qlat 26532 → 30873`, `+16%`), and
throughput regresses because each miss now takes longer to return.
This is the Little's-Law tell: the cap isn't the knob; the round-trip
is.

**What this implies for next steps**

- Widening L1D TBEs, L2 TBEs, or HNF TBEs will not raise 16-thread
  throughput on this mesh — all three have headroom, the cap is
  mesh round-trip.
- Real levers: (a) faster routers (`router_latency` = `4` cycles
  in `rbook_4x4.py`, could be lowered for a what-if), (b) wider
  links (`link_width_bits` = `128`, try `256` for a 2x DAT
  payload/cycle), (c) shorter Manhattan distance (a `2x2` topology
  instead of `4x4`), (d) more home-node slices per column so REQs
  cross fewer hops on average.
- The `--per-vnet-links` benefit already measured here (`+13.4%`)
  is roughly the best Garnet can offer on link arbitration alone
  for this CHI config at this working-set size.

`v1b` (L1D TBE=32) is kept as the canonical operating point because
raising the cap only makes things worse and gives misleadingly low
TBE utilization numbers.

### Lessons learned

1. A workload must overflow private L2 or single-thread windows see no
   mesh traffic at all (v1a).
2. HNF uniformity is a prerequisite for a link-bound study — if traffic
   concentrates on one HNF, that HNF's service rate saturates before
   the links and `--per-vnet-links` has nothing to relieve
   (hotspot / hotspot-heavy).
3. `--per-vnet-links` wins when (a) mesh is loaded and (b) 2+ vnets
   have substantial shared-link traffic. In v1b at 16 threads, REQ
   (19%), SNP (19%), DAT (62%) together flood each link — per-vnet
   separation removes the DAT head-of-line block and regains +13.4%.
4. Per-vnet benefit plateaus once mesh link contention is fully
   saturated. v2 (8 MiB) shows the same `+13%` as v1b (4 MiB); a
   larger working set raises raw LLC traffic but does not increase
   the vnet-contention headroom `--per-vnet-links` can recover.

---

## Complement to hotspot and hotspot-heavy

| Workload | HNF spread | Bottleneck | `--per-vnet-links` gain |
|---|---|---|---|
| `hotspot-heavy` (1 thread, HNF15 only) | all HNF15 | HNF15 service rate | **−0.02%** (noise) |
| `hotspot` (16 threads, HNF15 only) | all HNF15 | HNF15 service rate | **+0.2% to +0.8%** |
| `link-pressure` (16 threads, all HNFs) | `1.002` (uniform) | mesh links | **+13.4%** |

Same mesh, same CPU config — only the access pattern changes. The measured
`--per-vnet-links` impact is nearly zero, small, or large depending on
whether the **binding** constraint sits at the home node or on the mesh
links between tiles.
