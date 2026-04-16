# Hotspot Heavy

## Goal

`hotspot-heavy` is a single-thread remote-memory microbenchmark.

Its purpose is to maximize the cache-line rate that one O3 CPU can drive to a
remote CHI home node, specifically `HNF15`.

The primary optimization targets are:

- remote `HNF15` demand accesses per cycle
- measured line operations per cycle
- sustained network activity caused by that single CPU

## Method

The benchmark runs only on CPU `0` and measures one isolated stats window.

It allocates one or more private anonymous buffers.
Within each page it touches four cache lines at offset `0x3c0`, `0x440`,
`0x4c0`, and `0x540`.
In the Chapter 17 mesh config those lines home at `HNF15`.

The access shape is configurable:

- `READ_STREAMS`: number of private read streams
- `WRITE_STREAMS`: number of private write streams
- `PAGES`: number of pages per stream

For the default case of `1` read stream and `0` write streams, the benchmark
uses the same explicitly unrolled fast path as the original `hotspot`
single-thread case.
That keeps the default comparison fair and avoids losing performance to a more
generic loop structure.

O3 and CHI tuning through the shared mesh config:

- `CPU_LQ_ENTRIES`, `CPU_SQ_ENTRIES`, `CPU_ROB_ENTRIES`, `CPU_IQ_ENTRIES`
- `L1D_SEQ_OUTSTANDING` (RubySequencer `max_outstanding_requests`)
- `L1D_TBES`, `L2_TBES`, `HNF_TBES` (CHI transaction-buffer-entry counts)

## Current Result

Latest run (new defaults, see _Investigation history_ below):

- outdir: `m5out/rbook-hotspot-heavy-20260416-220216`
- config: `PAGES=2048`, `READ_STREAMS=1`, `WRITE_STREAMS=0`
- knobs (all defaults in `rbook_mesh_config.py`):
  - `l1d_seq_outstanding=32`, `l1d_tbes=32`, `l2_tbes=64`, `hnf_tbes=64`
  - `cpu_lq_entries=64`, `cpu_iq_entries=128`
  - SQ=32, ROB=192, physical regs 256 (framework defaults)
- **line ops/cycle: `0.064381` (+15.3% over the prior stock-default baseline)**
- cycles: `127 243` (was `146 702`)
- `HNF15` accesses/cycle: `0.0645`
- total network flits/cycle: `1.617`
- `HNF15` ext-link flits/cycle: `0.583`

Prior stock-default baseline (for comparison): line ops/cycle `0.055841`.
Reference single-thread `hotspot` baseline: line ops/cycle `0.055099`.

## Build

```bash
make -C ruby-book/final/hotspot-heavy build
```

## Run

```bash
make -C ruby-book/final/hotspot-heavy report   # run + check + Markdown report
make -C ruby-book/final/hotspot-heavy run      # run only
```

## Common Variants

```bash
# Restore the upstream stock widths (no CLI flags revert to framework defaults
# only if the config's own defaults are also None — these no longer are).
make -C ruby-book/final/hotspot-heavy report \
    L1D_SEQ_OUTSTANDING=16 L1D_TBES=16 L2_TBES=32 HNF_TBES=32 \
    CPU_LQ_ENTRIES=32 CPU_IQ_ENTRIES=64

# Push further (expected to regress - HNF15 queueing eats the gain):
make -C ruby-book/final/hotspot-heavy report \
    L1D_SEQ_OUTSTANDING=64 L1D_TBES=64 HNF_TBES=64 \
    CPU_LQ_ENTRIES=128 CPU_IQ_ENTRIES=256 CPU_ROB_ENTRIES=512

# Different stream shapes:
make -C ruby-book/final/hotspot-heavy report READ_STREAMS=2 WRITE_STREAMS=0
make -C ruby-book/final/hotspot-heavy report READ_STREAMS=1 WRITE_STREAMS=1
make -C ruby-book/final/hotspot-heavy report PAGES=4096

# Per-vnet internal links:
make -C ruby-book/final/hotspot-heavy report PER_VNET_LINKS=1
```

## Outputs

- benchmark: `rbook_test_hotspot_heavy.c`
- checker: `check_hotspot_heavy.py`
- report generator: `report_hotspot_heavy.py`
- report: `ValidatedRunReport.md`
- mesh config: `configs/example/rbook_mesh_config.py`

---

# Bottleneck Analysis Methodology

Before changing any knob, **find the bottleneck**. Random sweeps only move the
needle when they touch the layer that is currently binding throughput. The
six-step playbook walks from the CPU pipeline out to DRAM, at each layer
asking a single question whose answer points to the next layer to inspect.
The target metric is `line ops/cycle`.

A crucial lesson from the investigation history below: **stat occupancy that
happens to equal a slot count does not prove that slot count is the cap**.
You must verify by widening the slot count and watching the occupancy grow.
If the occupancy does not rise when you give it more room, the cap is
elsewhere - something upstream is throttling the fill rate.

## Step 0 - sanity: is this run memory-bound?

- `system.cpu0.cpi` / `system.cpu0.ipc` - overall rate.
- `system.cpu0.numIssuedDist::0` - fraction of cycles issuing zero insts.
  > 50% means the CPU is stalled most cycles.
- `system.cpu0.commit.numCommittedDist::0` - same question from retire side.
- `system.cpu0.lsq0.loadToUse::mean` - compare to L1D hit latency (1-3 cy)
  vs typical remote-HNF round-trip (~280-500 cy). Near-RT = memory-bound.

## Step 1 - which pipeline resource fills first?

Rename is the choke point in O3. Which downstream queue blocks it?

- `system.cpu0.rename.status::{Running,Blocked,Unblocking}` - fraction of
  cycles in each state.
- `system.cpu0.rename.{LQFullEvents,SQFullEvents,IQFullEvents,ROBFullEvents,fullRegisters}`
  - whichever is largest names the structural bottleneck at dispatch.
- Cross-check with `system.cpu0.iew.{lsqFullEvents,iqFullEvents,memOrderViolationEvents}`.

**Trap:** a big `rename.IQFullEvents` paired with tiny `iew.iqFullEvents`
means rename blocks before the IQ fills in IEW - the IQ is still the cause,
but rename sees it first.

## Step 2 - occupancy of the guilty queue

- `system.cpu0.lsq0.{lqAvgOccupancy,sqAvgOccupancy}` - > 0.9 confirms
  saturation.
- `system.cpu0.lsq0.blockedByCache` vs `commitStats0.numLoadInsts` - if
  every load was blocked at least once, the LSQ is full because the cache
  layer below it will not accept more requests.
- Compare against configured sizes in `config.ini` (`LQEntries`, `SQEntries`,
  `numROBEntries`, `instQueues.numEntries`).

## Step 3 - L1D MSHRs (TBEs) and the Ruby sequencer cap

This is the layer that most sweeps miss. In the CHI stack a "miss slot" is
a TBE. In Ruby there is also a **sequencer-level outstanding-request cap**
upstream of TBEs that is easy to overlook:

- `system.cpu0.l1d.TBEs.avg_size` / `...avg_util` - L1D miss slots used.
- `system.cpu0.l2.TBEs.avg_size` / `...avg_util` - same for private L2.
- `system.ruby.hnf<N>.cntrl.TBEs.avg_size` / `...avg_util` - the home node
  that owns the hot addresses.
- **`config.ini: max_outstanding_requests`** on `system.cpu0.data_sequencer`
  (RubySequencer) - gates how many demand requests the L1D sequencer will
  accept concurrently. Default is 16. If L1D TBE slots appear fully
  utilised at exactly 16, check this number first.

Arithmetic cross-check:

$$\text{MLP cap} \approx \frac{\text{avg TBE size at binding layer}}{\text{miss round-trip (cy)}}$$

If measured `HNF<N> accesses/cycle` matches this formula within a few
percent, you have found the binding layer. If not, some other gate is
limiting the fill rate; widen the suspected bind by 2x and verify that the
occupancy rises in proportion.

## Step 4 - miss latency decomposition

If a miss round-trip is long, split it:

- `system.cpu0.l1d.outTransLatHist.SendReadShared::mean` - full L1D miss
  round-trip.
- `system.cpu0.l1d.inTransLatHist.Load::mean` - load-servicing time at L1D,
  includes any retries.
- `system.ruby.network.average_flit_network_latency`,
  `average_flit_queueing_latency` - flits on wire vs flits in router
  buffers.
- `system.ruby.hnf<N>.cntrl.cache.m_demand_{hits,misses}` - does HNF hit or
  go to DRAM?

## Step 5 - network saturation

- `system.ruby.network.avg_link_utilization` - < 20% = plenty of headroom.
- Per-router `buffer_{reads,writes}` - identify hot router.
- `average_flit_{vnet,vqueue}_latency` - which vnet is queueing.

## Step 6 - front-end / back-end sanity

- `system.cpu0.fetchStats0.icacheStallCycles` / `numCycles`.
- `system.cpu0.statFuBusy::*` - for a pure-load workload, `MemRead` on top
  is **expected and benign** (it reflects LSQ-full retry, not FU scarcity).
- `system.cpu0.branchPred.mispredicted_*` - tiny for counted loops.

## Special case: single-remote-HNF hotspots

If the benchmark directs every access to one HNF (as `hotspot-heavy` does),
**HNF-side queueing becomes the ultimate cap** well before CPU-side
structures saturate. Symptoms:

- `hnf<N>.cntrl.TBEs.avg_size` close to `number_of_TBEs`, and
  `...avg_reserved` > 0 (TBEs being reserved for replacement/snoop).
- `l1d.outTransLatHist.SendReadShared::mean` grows sub-linearly faster than
  L1D `TBEs.avg_size` when you widen CPU-side structures - classic
  queueing behaviour ($L = \lambda W$).

In this regime, widening CPU-side knobs alone **regresses** throughput
because concurrency gains are swallowed by per-miss latency growth.
Widening HNF TBEs first (or splitting traffic across more HNFs) unblocks
further scaling.

## Summary decision table

| Symptom | Binding layer | What to change first |
| --- | --- | --- |
| `numIssuedDist::0` > 50%, `loadToUse::mean` >> 10 cy | memory-bound | - |
| `rename.LQFullEvents` dominates, LQ > 0.9, L1D TBE = 1.0, sequencer `max_outstanding_requests` = TBE count | **RubySequencer** is the real cap | raise `--l1d-seq-outstanding` (or match TBE to a larger value) |
| LQ full and TBE slot count > sequencer cap | sequencer confirmed | raise `--l1d-seq-outstanding`, then LQ |
| Sequencer & TBEs widened, miss RT grows with concurrency, HNF TBE `avg_reserved` > 0 | HNF / single-remote-HNF queueing | raise `--hnf-tbes`, or rehome traffic |
| `rename.IQFullEvents` dominates after sequencer/LQ fixes | IQ | raise `--cpu-iq-entries` |
| ROB stalls alone (LQ/IQ fine) | ROB | raise `--cpu-rob-entries` |
| `avg_link_utilization` > 0.6, hot router | network | vnet links, topology |
| `hnf.m_demand_misses / m_demand_accesses` ≈ 1 and RT ≈ DRAM | DRAM | memory config |

## Current Diagnosis (run `rbook-hotspot-heavy-20260416-220216`)

With new defaults (seq=32 / L1D TBE=32 / L2 TBE=64 / LQ=64 / IQ=128 / HNF=64):

- CPU is still memory-bound but well-utilised:
  - `numIssuedDist::0` = ~76%, `commit.numCommittedDist::0` = ~90%
  - `lsq0.loadToUse::mean` = 476 cy (up from 289 cy at baseline)
- L1D MSHR pool is now the tight layer:
  - `l1d.TBEs.avg_util` = **0.969** of `number_of_TBEs=32` (~31 outstanding)
  - `max_outstanding_requests` = 32 on data sequencer (matched)
- LQ is near-full (LQ=64 is the new cap on lane count):
  - `lsq0.lqAvgOccupancy` = **0.93** (~60 of 64 LQ entries busy)
  - `rename.LQFullEvents` = ~17k (dominant rename-blocker)
- IQ stalls are gone: `rename.IQFullEvents` = 0, `iew.iqFullEvents` = 0
- HNF has headroom:
  - `hnf15.cntrl.TBEs.avg_size` = 36 of 64 (util 0.56)
  - `hnf15.cntrl.TBEs.avg_reserved` = 0 (no snoop/repl pressure)
- Network: `avg_link_utilization` ≈ 8-9%

Arithmetic cross-check:

$$\frac{31.0\,\text{TBEs}}{481\,\text{cy miss RT}} = 0.0644\,\text{ops/cy}$$

Matches measured **0.0644** exactly. New binding layer is the paired
L1D TBE = sequencer cap = 32. Going higher regresses (see Point H below).

## Remaining Headroom

Three distinct pressures keep this config from going faster:

1. **HNF15 queueing grows super-linearly** with concurrency. Going to
   TBE=48 (Point H) already grew `SendReadShared::mean` to 668 cy, so
   even though 43 TBEs were in flight, throughput fell back to 0.0641.
2. **LQ≈0.93 full** but widening LQ alone at this TBE size did nothing
   (the smoke test). LQ is a symptom here, not the cause.
3. **Serial address-computation chain** in the unrolled loop means each
   load comes with ~2 dependent IntAlu ops, which occupy IQ slots until
   the load's base register is computed.

Further gains likely require **changing the workload distribution**:
spread traffic across more HNFs (for example by touching 2+ homes per
page) so HNF queueing no longer dominates, or add an L1D prefetcher to
convert demand misses into retired misses before HNF queues up.

---

# Investigation History

The journey to the current +15.3% config took several surprise
corrections. Keeping the trail here so readers can follow the
methodology, not just the result.

## v1 - initial diagnosis (wrong layer, right number)

First stats pull on the stock config:

- `l1d.TBEs.avg_util = 0.972` of `number_of_TBEs = 16` - looks binding.
- `lsq0.lqAvgOccupancy = 0.97` of `LQEntries = 32` - looks binding.
- Miss RT 278 cy, measured 0.0558 line ops/cy, arithmetic 15.55/278 =
  0.0559 - matches almost exactly.

Conclusion at the time: "L1D TBE count (16) is the MLP cap, LQ is
secondary." Recommended raising `number_of_TBEs` first.

## v2 - smoke test: raising LQ alone

Methodology predicted that raising LQ without touching TBEs would do
nothing. Ran `CPU_LQ_ENTRIES=128 CPU_SQ_ENTRIES=128 CPU_ROB_ENTRIES=512`
at stock TBE=16:

- Result: 0.05661 line ops/cy (+1.4%). Essentially flat. ✅
  Confirmed LQ was a symptom, not the cause.

## v3 - first contradiction: raising L1D TBE did NOT raise TBE occupancy

Wired `--l1d-tbes` / `--l2-tbes` CLI knobs and ran TBE=32 / LQ=64:

- `l1d.TBEs.avg_size` stayed at **15.56** (same as baseline).
- `l1d.TBEs.avg_util` fell to 0.486 (because the denominator grew).
- Throughput barely moved (0.05681 line ops/cy, +1.7%).

This **falsified v1's diagnosis**. If L1D TBEs were the cap, occupancy
would have risen when given more room. Something upstream was rate-
limiting the fill.

## v4 - finding the real gate: RubySequencer.max_outstanding_requests

Searched `config.ini` for other 16-valued structures and found
`max_outstanding_requests=16` on `system.cpu0.data_sequencer`. Looked
up `src/mem/ruby/system/Sequencer.py:96` - `Param.Int(16, "max
requests (incl. prefetches) outstanding")`. This gate sits upstream of
L1D TBEs and caps how many demand requests the sequencer will accept
concurrently, independent of LQ or TBE size.

The original 0.97 TBE utilisation was a **coincidence**: the sequencer
cap and TBE count were both 16 by default.

## v5 - widening the sequencer alone also regressed (second surprise)

Wired `--l1d-seq-outstanding` and ran seq=64 / L1D TBE=64 / L2=64 with
LQ=128 / ROB=512:

- `l1d.TBEs.avg_size` rose to 30.73 (now actually growing). ✅
- `SendReadShared::mean` blew up from 278 cy to **570 cy**.
- Throughput **regressed to 0.05384** (-3.6%).

The new bottleneck was HNF15 queueing. `hnf15.cntrl.TBEs.avg_reserved
= 10.88` showed the home node was reserving slots for replacement /
snoop that it couldn't serve yet. More upstream concurrency produced
longer queues downstream.

## v6 - widening HNF TBEs unblocked the scaling

Added `--hnf-tbes` knob and ran seq=64 / L1D TBE=64 / LQ=128 / HNF=64:

- `l1d.TBEs.avg_size` = 30.69, `SendReadShared::mean` = 503 cy,
  `hnf15.TBEs.avg_reserved` ≈ 0.
- Throughput **0.06092** (+9.1% over baseline).

This was the first genuine improvement. Widening HNF had done nothing
in v1's plan because we had not first widened the sequencer; widening
the sequencer without HNF regressed because queues shifted downstream.
Both had to move together.

## v7 - adding IQ uncovered a third bind

At the v6 config, `rename.IQFullEvents = 18984` with IQ=64 default
showed IQ was now the dispatch-side bottleneck.

- v7a: seq=64 / TBE=64 / LQ=128 / IQ=256 / HNF=64 → 0.05945 (-2.4%
  vs v6). Going bigger on everything at once over-pressured HNF
  again. `SendReadShared::mean` = 876 cy.
- v7b: seq=32 / TBE=32 / LQ=64 / IQ=128 / HNF=64 →
  **0.06438 (+15.3%)**. Moderate widening found the knee.

## v8 - verified the knee and set new defaults

- v8a: seq=48 / TBE=48 / LQ=96 / IQ=128 / HNF=64 / L2=64 → 0.06409
  (+14.8%). Past the knee - marginal regression.
- **Picked v7b as the new default** in `rbook_mesh_config.py`. Re-ran
  from defaults and reproduced 0.06438 exactly.

## Sweep summary

| ID | seq | L1D TBE | L2 TBE | HNF TBE | LQ | IQ | ROB | Line ops/cy | Δ vs stock |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| stock (v1) | 16 | 16 | 32 | 32 | 32 | 64 | 192 | 0.05584 | - |
| smoke (v2) | 16 | 16 | 32 | 32 | 128 | 64 | 512 | 0.05661 | +1.4% |
| v3 | 16 | 32 | 64 | 32 | 64 | 64 | 256 | 0.05681 | +1.7% |
| v5 A | 64 | 64 | 64 | 32 | 32 | 64 | 192 | 0.05615 | +0.6% |
| v5 B | 64 | 64 | 64 | 32 | 128 | 64 | 512 | 0.05384 | -3.6% |
| mid | 32 | 32 | 64 | 32 | 64 | 64 | 192 | 0.05420 | -2.9% |
| v6 D | 64 | 64 | 64 | 64 | 128 | 64 | 512 | 0.06092 | +9.1% |
| v7a E | 64 | 64 | 64 | 64 | 128 | 256 | 512 | 0.05945 | +6.5% |
| F | 32 | 32 | 64 | 64 | 64 | 64 | 192 | 0.06114 | +9.5% |
| **v7b G (default)** | **32** | **32** | **64** | **64** | **64** | **128** | **192** | **0.06438** | **+15.3%** |
| v8a H | 48 | 48 | 64 | 64 | 96 | 128 | 192 | 0.06409 | +14.8% |

## Lessons from this investigation

1. **High occupancy does not prove binding.** `l1d.TBEs.avg_util =
   0.97` looked like the classic smoking gun, but widening the slot
   count did nothing - the real gate was upstream at the sequencer.
   Always verify by widening and watching occupancy.
2. **Bottlenecks shift in non-obvious order.** Fixing the sequencer
   exposed HNF queueing, fixing HNF exposed IQ-full. Each step
   revealed the next. Plan for a _sequence_, not a single knob.
3. **Single-remote-HNF hotspots have a latency-vs-concurrency knee.**
   Widening CPU-side structures without HNF widening regressed
   because $L = \lambda W$ scaled $W$ (latency) with $\lambda$
   (arrival rate). The knee is exactly where per-miss latency growth
   catches up with concurrency gain.
4. **The Ruby sequencer cap is an under-documented gate.** It defaults
   to 16 and is not exposed by the common mesh configs. The stats
   file shows it as `max_outstanding_requests=...` on the data
   sequencer in `config.ini`.
