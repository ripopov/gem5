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

With the DAT head-of-line block removed, the next binding constraint
at `1.10` line ops/cycle is the **L1D TBE pool** (MSHRs), *not* the
HNF service rate or physical link bandwidth. From the v1b 16-thread
per-vnet stats block:

| Resource | Cap | Avg size | Avg utilization |
|---|---:|---:|---:|
| L1D TBEs (per CPU, sampled cpu0/1/7/15) | 32 | 23.5–24.1 | **0.74–0.75** |
| L2 TBEs (per CPU) | 64 | ~23.3 | 0.36 |
| HNF TBEs (per HNF, sampled 0/5/10/15) | 64 | 12.0–16.7 | 0.19–0.26 |
| Sequencer max outstanding | 32 | ~24 (tracks L1D TBE) | ~0.75 |

Network-side, queueing-latency / network-latency = `26531 / 15209 =
1.74x` — flits still spend ~1.7 cycles waiting for a channel for
every cycle of actual transit, so per-vnet-links did not fully drain
the queues. But the HNFs are only ~20% occupied and L2 TBEs under
40%, so downstream of the L1D there is headroom: the 32-slot L1D
TBE cap is what gates MLP per core.

**What this implies for next steps**

- Widening `--l1d-tbes` beyond 32 would let each core hold more in
  flight. That lets throughput climb only until the mesh links
  resaturate or HNF TBEs fill — both currently have headroom, so
  the next sweep point is *probably* a further `+N%` at a higher L1D
  TBE budget. This has not been measured yet and is not part of v2.
- Widening HNF TBEs or adding more HNF slices would not help here:
  HNF occupancy is ~20% of capacity, not the binding resource.
- Removing or flattening the L1D TBE cap would expose the **mesh
  link bandwidth** as the real ceiling — at 1.1 line ops/cycle the
  mesh is carrying `62%` DAT traffic at `128 bits/cycle/link/vnet`,
  close to the physical saturation point.

The link-pressure workload has done its job: it reduced the network
to a predictable many-to-many streaming load so that `--per-vnet-links`
could move the HoL block out of the way, and with that gone the L1D
MSHR/TBE cap is now the thing to push on — the classic "fix one
bottleneck, find the next."

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
