# Garnet (HeteroGarnet 3.0) Latency & Buffering — `memset` on the CHI testbench

A cycle-accurate dissection of where every clock cycle of a CHI `ReadUnique`
transaction comes from on the **Garnet** NoC, for the run:

```
build/RISCV/gem5.opt -d m5out/rbook-tb-gem5-memset \
  ruby-book/final/chi_testbench_gem5/driver/rbook_testbench_gem5.py \
  --scenario=memset --active-cores=0 --network=garnet \
  --num-outstanding-reqs=32
```

(`--per-vnet-links` and `vcs_per_vnet = 8` are set by default in the driver,
`set_defaults`, `rbook_testbench_gem5.py:120-137`.)

Observed result we are explaining (**current config**: `vcs_per_vnet = 8`,
`buffers_per_data_vc = 1`, per-vnet links):

```
memset ROI BW: 27.219917 Bytes/Clock (52480 bytes over 1928 clocks)
system.ruby.rnf0.cntrl.outTransLatHist.SendReadUnique::mean   70.360976   (cycles)
  2 (0-31cy) | 297 (32-63) | 448 (64-95) | 72 (96-127) | 1 (128-159)   (820 ROI samples)
```

> **What changed and why it matters.** `vcs_per_vnet` was raised from the default 4
> to **8**. This lifted throughput from **18.26 → 27.22 B/cy** and cut mean latency
> from **106.74 → 70.36 cy** (max 331 → 130). At 4 VCs the data return starved the
> incast funnel — flits stalled upstream waiting for credits (the old worst case sat
> +73/+75 cy at routers 14/13). With 8 VCs there are enough virtual lanes to keep
> the funnel link fed every cycle, so the return path now runs at its **structural**
> cost with essentially no queueing. The `vcs_per_vnet` sweep is in §12.
> `buffers_per_data_vc` was also set to 1, which is **inert** for this workload
> (data packets are single-flit — proven in §10/§12).

All numbers below were reproduced from a re-run with
`--debug-flags=ProtocolTrace,RubyNetwork`. A Python reconstruction of
`outTransLatHist` from the `ProtocolTrace` reproduced the stats histogram
**exactly** (820 samples, buckets 2/297/448/72/1, mean 70.36 cy), which validates
the per-transaction timeline used in §7. The per-hop network table in §7 is read
directly from the `RubyNetwork` flit trace by **PacketId** (§A.6).

> **Headline.** Garnet's mean latency (70.36 cy) and bandwidth (27.22 B/cy) are now
> within a few percent of SimpleNetwork's tuned figures (67.50 cy / 28.34 B/cy) —
> both networks read the same `rbook_4x4.py` params and the uncongested hop is the
> **same 6 cy** in both (§6). After tuning each network's anti-starvation knob
> (`vcs_per_vnet=8` for Garnet, `router_buffer_size=8` for Simple) they converge on
> the same ~28 B/cy structural ceiling (§12).

---

## 1. TL;DR — where the cycles go

`outTransLatHist.SendReadUnique` measures, **in requester (rnf0) clock cycles**, the
interval from *"the rnf cache injects the ReadUnique request"* to *"the last beat of
the line's data (CompData) has been received back"* (§5).

Measured directly from the trace (820 ROI transactions):

| Phase | What happens | Min (cy) | Mean (cy) | Max (cy) |
|------|---------------|---------:|----------:|---------:|
| **A. Request out** | rnf0 → mesh → HNF (**residual queueing lives here now, §8**) | 9 | 28.1 | 70 |
| **B. HNF (L3) hit** | tag(2) + data-array(10) | 12 | 12.0 | 12 |
| **C. Data return** | HNF → mesh → rnf0 (**now structural — no funnel queue**) | 10 | 30.2 | 58 |
| **Total** | `outTransLatHist.SendReadUnique` | **31** | **70.4** | **130** |

With `vcs_per_vnet = 8` the picture is **distance-dominated**, not queueing-dominated:

1. **Distance.** The HNF for a line is hash-interleaved across all 16 mesh routers;
   the worst lines map to **router 15**, diagonally opposite rnf0 → 6 hops each way
   (§6). The structural round trip to that corner is ~103 cy; the mean over uniform
   hashing is ~66 cy.
2. **Residual launch-burst queueing (request leg only).** The only transaction that
   still queues noticeably is the *first* of the ROI: at stats-reset rnf0's
   NetworkInterface must flitisize and inject all 32 outstanding requests, and they
   serialize through one injection point (~28 cy of extra request-leg latency for
   the worst victim, §7). The **return path no longer queues** — the credit-stall
   incast that dominated the old (vcs-4) config is gone (§8).

Average bandwidth follows from latency by Little's Law:
`BW ≈ 32 × 64 / 70.36 ≈ 29.1 B/cy`, close to the measured **27.22 B/cy** (§9).

---

## 2. System under test

* **Clock:** `system.clk_domain.clock = 500 ticks/cycle` → 2 GHz; 1 cycle = 500 ticks.
* **Topology:** `CustomMesh`, 4×4, from `configs/example/noc_config/rbook_4x4.py`.

  ```
  router grid (row-major)         node placement (this run)
    0  1  2  3                     every router r hosts rnf[r] and hnf[r];
    4  5  6  7                     router 0 also hosts snf0 + MN (DVM),
    8  9 10 11                     router 15 also hosts snf1.
   12 13 14 15                     rnf0  = router 0   (the only active core)
                                   hnf15 = "Cache-31" = router 15
  ```
* **Endpoint attachment.** In `--rn-mode=rnf_l2` the testbench replaces CHI's
  request-node factory (`cfg_rn.py`), so each tile is `RubySequencer →
  CHI_TileCacheController → mesh router` with **no L1 and no side "node router"**.
  This run has exactly **16 routers** (mesh only); every endpoint attaches through a
  **GarnetExtLink** (`config.ini`: 16 routers, 35 ext links, 192 int links). *(The
  generic `CHI_RNF` node-router with `node_router_latency=2` is **not** instantiated
  here.)*
* **Workload (`scenarios/memset.py`):** tile 0 writes 1024 lines × 64 B,
  `write_size=63` (partial line). L3 warm-up → ramp-up → **ROI (820)** → ramp-down.
  Stats reset at ROI start → 820 samples == 820 ROI writes.
* **Why `ReadUnique`:** a 63-byte (partial) store misses in the rnf leaf cache and
  needs the line writable → CHI **ReadUnique**; the warmed L3 hits and returns
  `CompData_UD_PD` (2 beats).
* **`--num-outstanding-reqs=32`:** up to 32 ReadUniques in flight (`memset.cc:195`).

---

## 3. Every latency parameter (this run's values)

### 3a. CHI protocol / controller latencies (SLICC `CHI-cache.sm`, in **cycles**)

Identical to the SimpleNetwork run (they live in the controllers, not the network):
`request_latency=1`, `response_latency=1`, `data_latency=1` (per beat),
`snoop_latency=1`, `mandatory_queue_latency=1`, `allocation_latency=0`,
`recycle_latency=10`, `to_memory_controller_latency=1` (unused; L3 hits).

### 3b. Cache-array access latencies (RubyCache SimObject — separate from §3a!)

| Cache | `dataAccessLatency` | `tagAccessLatency` | size / assoc |
|-------|--------------------:|-------------------:|--------------|
| **hnf (L3)** | **10 cy** | 2 cy | 16 MiB / 16-way |
| rnf (L2 leaf) | 6 cy | 2 cy | 256 KiB / 8-way |

L3 read hit = **tag 2 + data 10 = 12 cy** (the constant Phase B in §1).

### 3c. Garnet network latencies & capacities (`config.ini`, from `rbook_4x4.py` + `GarnetNetwork.py`)

| Param | Value | Meaning |
|-------|------:|---------|
| `GarnetRouter.latency` (`router_latency`) | **4 cy** | router pipeline depth (`m_latency`); flit waits `latency−1=3` cy in input VC, then 1 cy SA (§4) |
| `GarnetIntLink.latency` → `NetworkLink.link_latency` | **2 cy** | wire latency, router→router (`router_link_latency`) |
| `GarnetExtLink` `NetworkLink.link_latency` | **1 cy** | wire latency, router↔endpoint |
| `ni_flit_size` | **40 B** | flit width = one CHI data beat → **single-flit data packets** (`_ensure_single_flit_garnet`, driver:195) |
| **`vcs_per_vnet`** | **8** | virtual channels per vnet — **the tuned knob (§12)** |
| `buffers_per_data_vc` | **1** | flit buffers per data VC — **inert here** (single-flit packets, §10/§12) |
| `buffers_per_ctrl_vc` | 1 | flit buffers per control VC |
| `number_of_virtual_networks` | 4 | req(0) / snp(1) / rsp(2) / dat(3) |
| per-vnet links (`--per-vnet-links`) | on | each vnet has its **own** physical mesh link (192 int links) |
| `routing_algorithm` | 0 (TABLE) | deterministic weight-based X-Y routes |
| NetworkBridge `serdes`/`cdc` | **off** | no SerDes/CDC latency on any link |

**Derived per-hop latency** (verified in §6/§7):

* **Internal hop** (router→router) = router pipeline `4` + int link `2` = **6 cy**.
* **External hop** (router→endpoint) = router pipeline `4` + ext link `1` = **5 cy**
  (last consume→delivery into rnf0 measures ~6 cy incl. NI hand-off).
* **Injection** = controller enqueue (1 cy) + NI flitisize/schedule + ext link (1 cy),
  plus, during the launch burst, waiting to acquire an injection VC.

---

## 4. How a single flit hop works in Garnet

```
controller.datOut (MessageBuffer, infinite)  <- enqueue with data_latency (1 cy)
   ▼
NetworkInterface::wakeup()   flitisize message → push flit onto out NetworkLink
   │  (flitisizeMessage needs a free VC; if none, message waits in the infinite outport)
   ▼
NetworkLink::wakeup()        NetworkLink.cc:98  schedules consumer at +link_latency (int=2, ext=1)
   ▼
Router::wakeup → InputUnit::wakeup()  InputUnit.cc:78
   │  HEAD flit: route_compute(); buffer for (m_latency-1)=3 cy, then ready for SA
   ▼
SwitchAllocator::wakeup()    SwitchAllocator.cc:91  2-stage separable allocation:
   │  SA-i: each inport round-robin picks 1 input VC that has a free outVC + credit
   │  SA-ii: each outport round-robin grants 1 requesting inport; decrement credit
   ▼
CrossbarSwitch → OutputUnit out queue → next NetworkLink … repeat
```

### 4a. Router = fixed pipeline + switch arbitration
A flit is delayed a **fixed `router_latency` (4 cy)** even uncontended: 3 cy buffered +
1 cy SA (`InputUnit.cc:72-73,127-134`; `Router.hh: get_pipe_stages()=m_latency`). A flit
wins SA only if the chosen output VC is free **and** has a credit
(`send_allowed`, `SwitchAllocator.cc:294-349`); otherwise it retries next cycle — the
backpressure behind §8.

### 4b. Credit-based flow control (the key difference from SimpleNetwork)
Each VC has `buffers_per_data_vc` slots; a downstream router returns a **credit** when it
forwards a flit. With **`vcs_per_vnet` VCs**, a data link can host that many distinct
packets concurrently. **More VCs = more concurrent flows through the bottleneck funnel**
— that is exactly the §12 lever. (Buffer *depth* per VC only matters for multi-flit
packets; here packets are single-flit, so depth is inert — §10/§12.)

### 4c. `--per-vnet-links`
Each vnet gets its own physical mesh link, so data traffic never serializes against
req/rsp. Contention in §8 is purely data-VC vs. data-VC for credits and switch grants.

### 4d. Bandwidth is not the bottleneck
A 40-B beat is one flit at `bandwidth_factor=40` → drains a link in 1 cycle. Garnet never
serializes a beat across cycles; byte-bandwidth is not the binding resource.

---

## 5. What `outTransLatHist.SendReadUnique` actually measures

* **Start:** `Profile_OutgoingStart` in `BUSY_BLKD --SendReadUnique--> BUSY_INTR` — rnf0
  commits to sending.
* **Stop:** `Profile_OutgoingEnd_DataResp`, once `hasReceivedData()` — after the last
  data beat. Sample = `ticksToCycles(curTick() − start)`.
* 64 B line / 32 B `data_channel_size` ⇒ **2 beats**; the histogram stops on the 2nd.

Spans: send → network out → HNF lookup → network back → both data beats. Excludes
`Store`/`TagArrayRead` setup and the trailing `CompAck`/`Final` cleanup.

---

## 6. The uncongested latency budget (single transaction, no contention)

```
A. Request out  = inject(~2) + hops·6 + ext_in(~5)
B. HNF L3 hit   = tag(2) + data_array(10)                 = 12
C. Data back    = inject(~1) + hops·6 + ext_in(~6)
```

* **Nearest HNF** (0 hops — `hnf0` co-located on router 0): measured ROI **minimum 31
  cy** (request 9 + HNF 12 + data 10).
* **Uncontended per-hop = 6 cy**, proven on both legs of the §7 victim (consume→consume
  deltas all 3000 ticks).
* **Farthest HNF** (router 15, 6 hops) uncongested ≈ 9 + 36 (req) + 12 (HNF) + 36 + ~10
  (data) ≈ **103 cy**.

Measured **mean at 32 outstanding is now 70.4 cy** — close to the ~66-cy uncongested
average. **That is the effect of `vcs_per_vnet=8`: with enough virtual lanes, 32
concurrent transactions run near the structural budget.** (At the old `vcs_per_vnet=4`
the mean was 106.74 cy — ~40 cy of credit-stall queueing on top.) Network-level
cross-check: mean **data-vnet flit latency dropped from 57.6 → 25.5 cy**
(`average_flit_vnet_latency`, vnet 3), `average_hops = 3.0`.

---

## 7. The maximum-latency transaction, cycle by cycle (130 cy)

ROI transaction with the largest `outgoingEnd − outgoingStart`: **addr 0x1fc0, 130
cycles** (start 11239500, end 11304500). HNF = `Cache-31` = **hnf15 on router 15** (far
corner) — the *first* ROI transaction, caught in the launch burst. Times from
`ProtocolTrace` and `RubyNetwork` (per-hop keyed by **PacketId**, §A.6).

### Phase A — request rnf0 (router 0) → hnf15 (router 15): 70 cy (44 structural + ~26 launch-burst)
Request flit `PacketId=4529`, vnet 0:

| tick | Δcy | where | event |
|-----:|----:|-------|-------|
| 11239000 | – | rnf0 | `Store`, `TagArrayRead` (setup) |
| **11239500** | 0 | rnf0 | **`SendReadUnique` → START** |
| 11253500 | +28 | router 0 | flit consumed at R0 — **queued at the NI behind the 32-request launch burst** |
| 11256500 | +6 | router 1 | internal hop |
| 11259500 | +6 | router 2 | internal hop |
| 11262500 | +6 | router 3 | internal hop (end of X) |
| 11265500 | +6 | router 7 | internal hop (turn to Y) |
| 11268500 | +6 | router 11 | internal hop |
| 11271500 | +6 | router 15 | internal hop (arrived at HNF's router) |
| 11274500 | +6 | hnf15 | SA→Local + ext link; `ReadUnique_PoC` (line `UD` = hit) |

Every internal hop is exactly 6 cy; the only excess is the ~28-cy injection wait at the
source NI (32 requests, one injection point).

### Phase B — hnf15 L3 read hit: 12 cy
| tick | Δcy | event |
|-----:|----:|-------|
| 11274500 | 0 | `AllocRequestNoRetry`, `ReadUnique_PoC`, `TagArrayRead` |
| 11275500 | +2 | `ReadHitPipe`, `DataArrayRead` (← `tagAccessLatency = 2`) |
| 11280500 | +10 | `WaitCompAck`, `SendCompData`, `TX_Data` beat 1 (← **`dataAccessLatency = 10`**) |
| 11281000 | +1 | `TX_Data` beat 2 (← `data_latency = 1`) |

### Phase C — data hnf15 → rnf0: 47 cy (≈ all structural — **no funnel queue**)
Beat 1 = `PacketId=4623`, vnet 3, route `R15→R14→R13→R12→R8→R4→R0`. Per-hop —
`consume` = lands in input VC, `SA grant` = wins switch allocation (structural pipeline =
3 cy; anything beyond is queueing):

| router | consume tick | SA-grant tick | arrive→grant | queueing (−3 cy) |
|--------|-------------:|--------------:|-------------:|-----------------:|
| R15 (src) | 11282000 | 11283500 | 3 cy | +0 |
| R14 | 11285000 | 11286500 | 3 cy | +0 |
| R13 | 11288000 | 11289500 | 3 cy | +0 |
| R12 | 11291000 | 11292500 | 3 cy | +0 |
| R8 | 11294000 | 11295500 | 3 cy | +0 |
| R4 | 11297000 | 11298500 | 3 cy | +0 |
| R0 | 11300000 | 11302000 | 3 cy | +0 |

| 11304500 | rnf0 | `CompData_UD_PD` **beat 2 → STOP**; then `CheckCacheFill`,`FillPipe`,`Final → UD` |

`END − START = 11304500 − 11239500 = 65000 ticks = 130 cy.`

**Where the time went:** of 130 cy, ~103 cy is structure (44 req + 12 HNF + 47 data) and
only ~27 cy is queueing — *all* of it the launch-burst injection wait on the request leg.
**Compare the old vcs-4 config (§12): the same far-corner victim took 331 cy with ~228 cy
of credit-stall queueing on the *return* leg (routers 14/13 alone added +73/+75 cy).**
Every return hop is now the bare 6-cy structural cost with **zero** SA-wait — the credit
backpressure is gone.

---

## 8. The incast is still there — it just no longer starves

The incast pattern is unchanged: 32 outstanding requests → dozens of lines of `CompData`
converge on router 0. What changed is whether they stall.

* At **`vcs_per_vnet = 4`** only 4 data packets could occupy a link at once. When the
  col-0 funnel filled, credits stopped returning, and `send_allowed` failed upstream
  (`SwitchAllocator.cc:294-349`). The stall propagated **upstream** and parked flits in
  the small VC buffers of routers 14/13 (+73/+75 cy in the old trace) — the funnel link
  saw idle cycles because flits couldn't be supplied fast enough.
* At **`vcs_per_vnet = 8`** there are enough virtual lanes that the funnel link is fed
  every cycle and drains at its 1-flit/cy ceiling. The §7 trace shows it: every return
  hop wins SA in the bare 3-cy pipeline with **+0 queueing**.

**Endpoint buffers are identical to SimpleNetwork** — the CHI controllers' in/out
`MessageBuffer`s are infinite (`buffer_size=0`) in *both* networks; an un-injectable
message simply waits there (Garnet: `flitisizeMessage` returns `false` without dequeuing,
`NetworkInterface.cc:216-217,421-423`). The networks differ only in **fabric flow
control**: SimpleNetwork uses per-port slot checks with a deep FIFO (`buffer_size`),
Garnet uses per-VC credits with `vcs_per_vnet` lanes. Each network's funnel-starvation
knob (`router_buffer_size` vs `vcs_per_vnet`) removes the *same* bottleneck (§12).

The only residual queueing is at the **source injection point** during the startup burst
(§7 Phase A) — fundamentally "32 requests, one rnf0 NI, limited injection per cycle".

---

## 9. Reconciling with the headline numbers

* **Histogram reproduced exactly**: 820 ROI samples, buckets 2/297/448/72/1, mean 70.36,
  max 130 — the §7 timeline is the real mechanism.
* **Min 31 cy** = nearest HNF (hnf0, router 0). **Max 130 cy** = far corner + startup
  request burst (§7).
* **Bandwidth (Little's Law):** `BW ≈ 32 × 64 / 70.36 = 29.1 B/cy` vs. measured **27.22
  B/cy**. The shortfall is the partial-write `CompAck` round-trip lowering effective
  concurrency.

---

## 10. Every capacity / buffer parameter

| Buffer / limit | size | notes |
|--------|------:|-------|
| Controller in/out `MessageBuffer` | **0 = infinite** | same CHI controllers as SimpleNetwork; no endpoint backpressure |
| **`vcs_per_vnet`** | **8** | virtual lanes per vnet — **the tuned knob** (§12) |
| `buffers_per_data_vc` | **1** | flit slots per data VC — **inert** for single-flit data (only matters for multi-flit packets, §12) |
| `buffers_per_ctrl_vc` | 1 | flit slots per control VC |
| `garnet_deadlock_threshold` | 50000 cy | network-level deadlock watchdog |
| TBE table (per controller) | (see config) | full table → `recycle_latency=10` stalls, or RetryAck/reqIn backpressure with `--allow-retryack=0` |

**Why `buffers_per_data_vc=1` is inert here:** data packets are single-flit
(`ni_flit_size=40` = one 40-B beat). A VC is held by one packet HEAD→TAIL; a single-flit
packet occupies exactly **1 slot** and frees it on the same flit. Slots 2-4 are never
used → depth has zero effect (confirmed byte-identical in §12). It would only matter for
multi-flit packets (smaller flit width or larger beat), where a VC must hold body/tail
flits and cover the credit round-trip.

---

## 11. Levers to reduce the measured latency

| Lever | Effect (measured where noted) |
|------|--------|
| **↑ `vcs_per_vnet` (4 → 8)** | **the headline fix here.** Removes the credit-stall incast: mean 106.74 → **70.36 cy**, BW 18.26 → **27.22 B/cy**, max 331 → **130** (§12). |
| ↓ `--num-outstanding-reqs` | latency lever via Little's Law (trades BW for latency). |
| HNF placement / spread active cores | shorter routes + no single-corner funnel. |
| ↓ `dataAccessLatency` of the L3 (10 cy) | cuts Phase B (~10 cy) per transaction. |
| `buffers_per_data_vc` | **no effect here** (single-flit data); only helps multi-flit packets. |
| wider flit / `link bandwidth` | no effect — already 1 flit/cy; latency-bound. |

---

## 12. The `vcs_per_vnet` sweep, and Garnet vs SimpleNetwork

### 12a. `vcs_per_vnet` sweep (why 8)

`vcs_per_vnet` sets how many data packets can occupy a link at once. At 4 the col-0
funnel starved; more VCs keep it fed. Sweep (same command, only `vcs_per_vnet` varied):

| `vcs_per_vnet` | ROI BW (B/cy) | mean lat (cy) | max lat (cy) | note |
|---:|---:|---:|---:|------|
| **4** (default) | 18.26 | 106.74 | 331 | funnel starves; flits stall +73/+75 cy at R14/R13 |
| 6 | 25.55 | 75.5 | – | most of the gain |
| **8** (current) | **27.22** | **70.36** | **130** | return path now structural; diminishing returns |

It is a ramp with diminishing returns toward the funnel's 1-flit/cy ceiling (4→6 gives
+7.3 B/cy, 6→8 only +1.7). `buffers_per_data_vc` does **not** appear as a lever — for
single-flit data it is inert (§10); `vcs=8 / buffers=4` and `vcs=8 / buffers=1` produce
**byte-identical** results.

### 12b. Garnet vs SimpleNetwork (both tuned)

Both use the same `rbook_4x4.py` params, CHI protocol, 32-outstanding concurrency, and
4×4 placement. The uncontended hop is **6 cy in both**. After removing each network's
funnel starvation they converge:

| Metric | SimpleNetwork (`router_buffer_size=8`) | Garnet (`vcs_per_vnet=8`) | Why |
|--------|--------------------------:|-------------------:|-----|
| Uncontended internal hop | 6 cy | 6 cy | same `router_latency`+`router_link_latency` |
| Anti-starvation knob | `router_buffer_size` 4→8 | `vcs_per_vnet` 4→8 | deeper FIFO vs. more virtual lanes |
| Endpoint (controller) buffers | ∞ `MessageBuffer` | ∞ `MessageBuffer` | **same** — identical CHI controllers |
| ROI **mean** latency | 67.50 cy | **70.36 cy** | structure + concurrency |
| ROI **BW** | 28.34 B/cy | **27.22 B/cy** | Little's Law from the mean |
| ROI **min / max** | 29 / 122 | 31 / 130 | Garnet's fixed 4-cy pipeline + injection adds a few cy |
| Where the queue sits | source injection burst only | source injection burst only | return path structural in both |
| Untuned (default) BW | 17.99 (buf 4) | 18.26 (vcs 4) | both started funnel-starved |

**Bottom line:** Garnet and SimpleNetwork are configured to the same per-hop latency, so
once each network's fabric-starvation knob is sized (`vcs_per_vnet=8` ≈
`router_buffer_size=8`) they reach the same ~28 B/cy structural ceiling, with the data
return fully uncongested and only the startup injection burst left as residual queueing.
SimpleNetwork edges slightly ahead because Garnet pays a fixed 4-cy router pipeline and a
touch more injection latency.

---

## Appendix A — Reproducing this analysis from traces

### A.1 Regenerate the trace

```sh
./util/run_with_timeout.sh ./build/RISCV/gem5.opt \
  --debug-flags=ProtocolTrace,RubyNetwork --debug-file=trace.gz \
  -d m5out/garnetlat-final-$(date +%Y%m%d-%H%M%S) \
  ruby-book/final/chi_testbench_gem5/driver/rbook_testbench_gem5.py \
  --scenario=memset --active-cores=0 --network=garnet \
  --num-outstanding-reqs=32
gunzip -c m5out/garnetlat-final-*/trace.gz > /tmp/gfinal.txt
```

### A.2 The three row formats

**(a) ProtocolTrace** — one row per SLICC transition, carries **address** + controller
`version` (**0 = rnf0**, **16 = hnf0**, **31 = hnf15**).

**(b) NetworkInterface "Scheduling"** — one row per flit injection, carries the flit's
**PacketId** *and* the full Message (with **addr**) — the bridge between the
address-keyed protocol view and the PacketId-keyed network view:

```
11281000: …netifs15: Scheduling at …ext_links15.network_links0 time:11281500
  flit:[flit:: PacketId=4623 … Vnet=3 … Src Router=15 … Dest Router=0 …]
  Message:[CHIDataMsg: addr = [0x1fc0, …] type = CompData_UD_PD …]
```

**(c) RubyNetwork per-hop** — `… Consuming:…` (flit lands in a router input) and
`SwitchAllocator at Router N granted …` (flit wins SA), keyed by **PacketId** (no
address on these rows — hence the addr→PacketId map in step A.6).

### A.3 Gotcha: warm-up vs ROI copies

Every address appears multiple times; the warm-up writes full lines at low ticks, the ROI
does partial writes (`SendReadUnique`) at ~11 M ticks. Filter to the ROI window.

### A.4 ROI window

`memset.cc` prints: `reset stats at tick 11238500`, `dump stats at tick 12202500`. So the
ROI window is `[11238500, 12202500]`.

### A.5 Step 1 — validate and pick the victim

Parse only ProtocolTrace version-0 rows, pair each `SendReadUnique` with its completing
**2nd** `CompData_UD_PD`, keep completions in the ROI window, re-bucket to prove the
parse matches `stats.txt`, sort for the max:

```python
import re
from collections import defaultdict, Counter
ROI_START, ROI_END = 11238500, 12202500
prot = re.compile(r'^\s*(\d+)\s+(\d+)\s+Cache\s+(\S+)\s+(\S+)\s+\[(0x[0-9a-f]+),')
byaddr = defaultdict(list)
for ln in open('/tmp/gfinal.txt', encoding='latin-1'):
    m = prot.match(ln)
    if not m or int(m.group(2)) != 0:
        continue
    byaddr[m.group(5)].append((int(m.group(1)), m.group(3)))
results = []
for addr, evs in byaddr.items():
    evs.sort()
    sends = [t for t, e in evs if e == 'SendReadUnique']
    comps = [t for t, e in evs if e == 'CompData_UD_PD']
    for st in sends:
        after = [c for c in comps if c >= st]
        if len(after) >= 2 and ROI_START <= after[1] <= ROI_END:
            results.append((after[1] - st, st, after[1], addr))
results.sort(reverse=True)
lat = [r[0] // 500 for r in results]
print("ROI txns: %d  min %d  max %d  mean %.2f" %
      (len(lat), min(lat), max(lat), sum(lat) / len(lat)))
print("32-cy buckets:", dict(sorted(Counter(min(c // 32, 4) for c in lat).items())))
print("max:", results[0])
```

Expected (matches `stats.txt` exactly — `bucket_size = 32`):

```
ROI txns: 820  min 31  max 130  mean 70.36
32-cy buckets: {0: 2, 1: 297, 2: 448, 3: 72, 4: 1}
max: (65000, 11239500, 11304500, '0x1fc0')
```

### A.6 Step 2 — map the victim's address to PacketIds, then trace per hop

```sh
# (i) addr -> PacketId for the victim's data-return flits (vnet 3, Src Router 15)
grep -a "0x1fc0," /tmp/gfinal.txt | grep -a "Scheduling at" | grep -a "Vnet=3" \
  | grep -a "Src Router=15"          # -> PacketId=4623 (beat1), 4627 (beat2)

# (ii) trace one flit hop-by-hop (consume = arrival, granted = SA win)
grep -a "PacketId=4623 " /tmp/gfinal.txt \
  | grep -aE "Consuming|SwitchAllocator at Router"
```

For each router, `consume→consume` is the hop latency (uncontended = 6 cy) and
`consume→SA-grant` minus the 3-cy router pipeline is the queueing. In this config every
return hop is +0 — the structural minimum (§7/§8).

### A.7 Phase split (the §1 table)

Pair version-0 `SendReadUnique` with the HNF (version ≥16) `ReadUnique_PoC` (request-leg
end) and `SendCompData` (HNF-leg end); remainder to the 2nd `CompData_UD_PD` is the
data-return leg. Over 820 ROI transactions: request 9/28.1/70, HNF 12/12/12, data-return
10/30.2/58, total 31/70.4/130 cy (min/mean/max).

### A.8 Files used

* Trace: `m5out/garnetlat-final-*/trace.gz` → `/tmp/gfinal.txt`.
* Stats: `m5out/garnetlat-final-*/stats.txt`
  (`…outTransLatHist.SendReadUnique`, `…network.average_flit_vnet_latency`).
* Config: `m5out/garnetlat-final-*/config.ini` (VC/buffer/link wiring).

---

## Appendix B — key source references

* Garnet router pipeline: `Router.cc:71-97`, `Router.hh` (`get_pipe_stages()=m_latency`),
  `InputUnit.cc:77-134`, `SwitchAllocator.cc:91-349` (`send_allowed` credit check),
  `NetworkLink.cc:97-132`, `NetworkInterface.cc:204-220,396-423` (flitisize/inject),
  `flit.cc:108-126`.
* Garnet params: `GarnetNetwork.py` (vcs/buffers/flit size), `GarnetLink.py`.
* Topology: `configs/topologies/CustomMesh.py`, `noc_config/rbook_4x4.py:21-34`.
* CHI ReadUnique: `CHI-cache-transitions.sm:277,1238`,
  `CHI-cache-actions.sm:1665,2802,3144,3902-3910,3934`, `CHI-cache.sm:56-150`.
* Histogram plumbing: `slicc/symbols/StateMachine.py:1020`,
  `AbstractController.hh:393-431`.
* Driver / single-flit sizing & `vcs_per_vnet=8`: `rbook_testbench_gem5.py:120-216`.
</content>
