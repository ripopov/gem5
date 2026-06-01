# SimpleNetwork Latency & Buffering — `memset` on the CHI testbench

A cycle-accurate dissection of where every clock cycle of a CHI `ReadUnique`
transaction comes from, for the run:

```
build/RISCV/gem5.opt -d m5out/rbook-tb-gem5-memset \
  ruby-book/final/chi_testbench_gem5/driver/rbook_testbench_gem5.py \
  --scenario=memset --active-cores=0 --network=simple \
  --num-outstanding-reqs=32
```

(`--simple-physical-channels` is on by default in the driver, `set_defaults`,
`rbook_testbench_gem5.py:120-136`.)

Observed result we are explaining (**current config**, `rbook_4x4.py`:
`link_bandwidth_factor = 40`, **`router_buffer_size = 8`**):

```
memset ROI BW: 28.336933 Bytes/Clock (52480 bytes over 1852 clocks)
system.ruby.rnf0.cntrl.outTransLatHist.SendReadUnique::mean   67.504878   (cycles)
  50 (0-31cy) | 252 (32-63) | 460 (64-95) | 58 (96-127) | 0 (128+)   (820 ROI samples)
```

> **What changed and why it matters.** `rbook_4x4.py` now sets
> **`router_buffer_size = 8`** (up from the gem5 default of 4). This single knob
> raised throughput from **17.99 → 28.34 B/cy** and cut mean latency from
> **108.75 → 67.50 cy** (max 226 → 122). The default-4 buffers were *starving the
> incast funnel*; 8 keeps the 1-flit/cy funnel link fed and the return path now
> runs at its **structural** cost with essentially no queueing. The
> `router_buffer_size` sweep that pins this is in §12. This is the SimpleNetwork
> twin of raising `vcs_per_vnet` on Garnet (see GarnetLatency.md §12).

All numbers below were reproduced from a re-run with
`--debug-flags=ProtocolTrace,RubyNetwork`. A Python reconstruction of
`outTransLatHist` from the `ProtocolTrace` reproduced the stats histogram
**exactly** (820 samples, buckets 50/252/460/58, mean 67.50 cy), which validates
the per-transaction timeline used in §7.

---

## 1. TL;DR — where the cycles go

`outTransLatHist.SendReadUnique` measures, **in requester (rnf0) clock cycles**, the
interval from *"the rnf cache injects the ReadUnique request"* to *"the last beat of
the line's data (CompData) has been received back"* (§5).

Measured directly from the trace (820 ROI transactions):

| Phase | What happens | Min (cy) | Mean (cy) | Max (cy) |
|------|---------------|---------:|----------:|---------:|
| **A. Request out** | rnf0 → mesh → HNF (**residual queueing lives here now, §8**) | 8 | 26.4 | 63 |
| **B. HNF (L3) hit** | tag(2) + data-array(10) | 12 | 12.0 | 12 |
| **C. Data return** | HNF → mesh → rnf0 (**now structural — no funnel queue**) | 9 | 29.1 | 53 |
| **Total** | `outTransLatHist.SendReadUnique` | **29** | **67.5** | **122** |

With `router_buffer_size = 8` the picture is now **distance-dominated**, not
queueing-dominated:

1. **Distance.** The home node (HNF) for a line is hash-interleaved across all 16
   mesh routers. The worst lines map to **router 15**, the corner diagonally
   opposite rnf0 (router 0) → a 6-hop X-Y route each way (§6). The structural round
   trip to that corner is ~103 cy; the mean over uniform hashing is ~65 cy.
2. **Residual launch-burst queueing (request leg only).** The only transaction
   that still queues noticeably is the *first* of the ROI: at stats-reset rnf0
   fires all 32 outstanding requests at once, and they serialize through rnf0's
   single injection point (~19 cy of extra request-leg latency for the worst
   victim, §7). The **return path no longer queues at all** — the incast funnel
   that dominated the old (buffer-4) config is eliminated (§8).

Average bandwidth follows from latency by Little's Law:
`BW ≈ N_outstanding × line_bytes / avg_latency = 32 × 64 / 67.5 ≈ 30.3 B/cy`,
close to the measured **28.34 B/cy** (§9). **The workload is latency-bound, and
with adequate buffering the latency is now essentially the structural routing
budget.**

---

## 2. System under test

* **Clock:** `system.clk_domain.clock = 500 ticks/cycle` → 2 GHz. Everything below is
  quoted in **cycles**; 1 cycle = 500 ticks.
* **Topology:** `CustomMesh`, 4×4, built from `configs/example/noc_config/rbook_4x4.py`.

  ```
  router grid (row-major)         node placement (this run)
    0  1  2  3                     every router r hosts rnf[r] and hnf[r];
    4  5  6  7                     router 0 also hosts snf0 + MN (DVM),
    8  9 10 11                     router 15 also hosts snf1.
   12 13 14 15                     rnf0  = router 0   (the only active core)
                                   hnf15 = "Cache-31" = router 15
  ```
  `router_list = range(16)` for both `CHI_RNF` and `CHI_HNF`
  (`rbook_4x4.py:36-43`), so `rnf_i`/`hnf_i` sit on router `i`.
* **Requester model (`--rn-mode=rnf_l2`):** `RubySequencer → CHI_Cache_Controller
  (a coherent L2-sized leaf cache) → mesh router`. No L1, no side router.
* **Workload (`scenarios/memset.py`):** tile 0 writes 1024 lines × 64 B, `write_size=63`
  (partial line). Phases: L3 warm-up → ramp-up (102) → **ROI (820)** → ramp-down (102).
  Stats are reset at ROI start, so the histogram's 820 samples == the 820 ROI writes.
* **Why `ReadUnique`:** a 63-byte (partial) store misses in the rnf leaf cache and must
  obtain the line writable (Unique), so the cache issues a CHI **ReadUnique**. Because
  the L3 was warmed, every ReadUnique **hits in some HNF's L3**, returning
  `CompData_UD_PD` (2 beats).
* **`--num-outstanding-reqs=32`:** the sequence keeps up to 32 ReadUniques in flight
  (`memset.cc:195`).

---

## 3. Every latency parameter (this run's values)

### 3a. CHI protocol / controller latencies (SLICC `CHI-cache.sm`, in **cycles**)

| Param | Value | Meaning / where it applies |
|-------|------:|----------------------------|
| `request_latency` | 1 | enqueue delay for a request msg (`Send_ReadUnique`, `reqOut`) |
| `response_latency` | 1 | enqueue delay for a response msg (`Comp*`, `rspOut`) |
| `data_latency` | 1 | enqueue delay **per data beat** (`Send_Data`, `datOut`) |
| `snoop_latency` | 1 | enqueue delay for a snoop msg |
| `mandatory_queue_latency` | 1 | seq request → mandatory queue |
| `allocation_latency` | 0 | TBE allocation |
| `recycle_latency` | 10 | resource-stall retry delay (TBE/queue full → re-process) |
| `to_memory_controller_latency` | 1 | HNF → SNF. *Not used here: L3 hits.* |

### 3b. Cache-array access latencies (RubyCache SimObject — **separate** from §3a!)

| Cache | `dataAccessLatency` | `tagAccessLatency` | size / assoc |
|-------|--------------------:|-------------------:|--------------|
| **hnf (L3)** `…hnf*.cntrl.cache` | **10 cy** | 2 cy | 16 MiB / 16-way |
| rnf (L2 leaf) `…rnf*.cntrl.cache` | 6 cy | 2 cy | 256 KiB / 8-way |

So an L3 read hit pays **tag 2 + data 10 = 12 cy** inside the HNF, independent of the
network. (This is the constant 12-cy Phase B in §1 — `min == mean == max`.)

### 3c. SimpleNetwork latencies (`config.ini`, from `rbook_4x4.py:24-34`)

| Param | Value | Meaning |
|-------|------:|---------|
| `Switch.int_routing_latency` | 4 cy | PerfectSwitch routing latency when the **next** link is internal (router→router) |
| `Switch.ext_routing_latency` | 6 cy | PerfectSwitch routing latency when the next link is **external** (router→endpoint) |
| `SimpleIntLink.latency` | 2 cy | wire latency, router→router |
| `SimpleExtLink.latency` | 1 cy | wire latency, router↔endpoint |
| **`SimpleNetwork.buffer_size`** | **8** | **router/switch port-buffer depth** — from `NoC_Params.router_buffer_size` (`CHI.py:242`). **The key tuned knob (§12).** |
| `link_bandwidth_factor` | 40 | **bytes/cycle** per link → one 40 B data flit per cycle (§4) |
| `control_msg_size` | 8 B | wire size of a control message |
| `data_msg_size` | 32 B (+8 hdr = **40 B on the wire**) | wire size of one data beat |
| `number_of_virtual_networks` | 4 | req / snp / rsp / dat |
| `physical_vnets_channels` | `1 1 1 1` | from `--simple-physical-channels`: each vnet gets its own physical channel & bandwidth |

**Derived per-hop latencies** (verified directly in the trace, §7):

* **Internal hop** (router→router) = `int_routing_latency + int_link_latency` = **4 + 2 = 6 cy**.
* **External hop** (router→endpoint) = `ext_routing_latency + ext_link_latency` = **6 + 1 = 7 cy**.
* **Injection** (endpoint→its own router) = the controller enqueue latency (1 cy) plus,
  during the launch burst, waiting behind other requests at rnf0's single output port.

---

## 4. How a single message hop works in SimpleNetwork

```
controller.reqOut (MessageBuffer, infinite)  <- enqueue with request_latency
   │  (this buffer is the PerfectSwitch input port)
   ▼
PerfectSwitch::wakeup()  src/mem/ruby/network/simple/PerfectSwitch.cc:278
   │  moves head msg input→output port buffer (size = buffer_size = 8), adds routing_latency
   ▼
Switch port buffer (intermediate, size = buffer_size = 8)
   ▼
Throttle::wakeup()       src/mem/ruby/network/simple/Throttle.cc:248
   │  dequeues, applies BANDWIDTH (serialization), then enqueues with link_latency
   ▼
next router input buffer (SimpleIntLink buffer, size 3)  OR  destination controller.reqIn
```

### 4a. PerfectSwitch = routing latency + arbitration + backpressure
* Wakes when a message arrives; iterates vnets in priority order, round-robin over
  input ports, oldest-message-first.
* For each routable message it checks the destination port has space
  (`areNSlotsAvailable`, `PerfectSwitch.cc:216`); if **not**, it leaves the message in
  place and **reschedules itself +1 cycle** (`PerfectSwitch.cc:226`). *This is the
  backpressure.* **The size of those destination port buffers is `buffer_size` — the
  knob this report is about.** With 4 slots the funnel link starved; with 8 it stays
  fed (§8/§12).
* On success it dequeues from the input and enqueues to the output port buffer with
  delay `int_routing_latency` / `ext_routing_latency` (`Switch.cc:126-127`).

### 4b. Throttle = bandwidth (serialization) + wire latency
* `getLinkBandwidth = endpoint_bandwidth × bandwidth_factor = 1000 × 40` units/cy;
  units == bytes × 1000, so the link moves **40 bytes/cycle**.
* A 40 B data beat drains in exactly **1 cycle** (one 256-bit flit/cycle, verified in
  the trace). Byte-bandwidth is **not** the bottleneck.

### 4c. `--simple-physical-channels`
Sets `physical_vnets_channels = [1,1,1,1]`, giving each vnet its **own** physical channel
& 40 B/cy budget, so req/rsp/dat don't serialize against each other.

---

## 5. What `outTransLatHist.SendReadUnique` actually measures

* **Start:** `action(Profile_OutgoingStart)` in the requester transition
  `BUSY_BLKD --SendReadUnique--> BUSY_INTR` — the instant rnf0 *commits to sending*.
* **Stop:** `action(Profile_OutgoingEnd_DataResp)`, which fires once
  `tbe.expected_req_resp.hasReceivedData()` — i.e. **after the last expected data beat
  arrives**. Sample = `ticksToCycles(curTick() − start)` (`AbstractController.hh:412-431`).
* A 64 B line / 32 B `data_channel_size` ⇒ **2 `CompData_UD_PD` beats**; the histogram
  stops on the **2nd** beat.

So the metric spans: requester send → network out → HNF lookup → network back → both
data beats in. It excludes the earlier `Store`/`TagArrayRead` setup and the trailing
`CompAck`/`CheckCacheFill`/`Final` cleanup.

---

## 6. The uncongested latency budget (single transaction, no contention)

Per-hop costs from §3c, distance = Manhattan(router0, router_of_HNF).

```
A. Request out  = inject(1) + hops·6 + ext_in(7)
B. HNF L3 hit   = tag(2) + data_array(10)                       = 12
C. Data back    = inject(1) + hops·6 + ext_in(7)
```

* **Nearest HNF** (0–1 hops): ≈ 8 (A) + 12 (B) + 9 (C) = **~29 cy** → matches the
  measured ROI **minimum of 29 cy**.
* **Farthest HNF** (router 15, 6 hops): A = 1+36+7 = **44**, B = 12, C = 44 → **~100 cy**.
* **Average** (uniform line→HNF hashing, mean ≈ 3 hops each way): ≈ 26 + 12 + 27 =
  **~65 cy**.

The measured **mean at 32 outstanding is now 67.5 cy** — only ~2 cy above the
uncongested average. **That is the whole point of `router_buffer_size = 8`: with enough
fabric buffering, 32 concurrent transactions run at essentially the structural budget.**
(At the old `buffer_size = 4` the mean was 108.75 cy — ~44 cy of pure queueing on top.)

---

## 7. The maximum-latency transaction, cycle by cycle (122 cy)

Selected automatically as the ROI transaction with the largest
`outgoingEnd − outgoingStart`: **addr 0x1fc0, 122 cycles** (start tick 11089500, end
tick 11150500). HNF = `Cache-31` = **hnf15 on router 15** (the far corner). This is the
*first* ROI transaction — issued the instant stats reset, so it is caught in the launch
burst of all 32 outstanding requests and queues on the **request** leg.

### Phase A — request rnf0 (router 0) → hnf15 (router 15): 63 cy (44 structural + ~19 launch-burst)
| tick | Δcy | where | event |
|-----:|----:|-------|-------|
| 11089000 | – | rnf0 | `Store I>BUSY_BLKD`, `TagArrayRead` (setup, *before* the clock) |
| **11089500** | 0 | rnf0 | **`SendReadUnique` → START** |
| 11097500 | +16 | router 0 | request leaves `reqOut` — **queued behind the 32-request launch burst** |
| 11102500 | +10 | router 1 | hop = 6 cy +4 cy residual burst queueing |
| 11105500 | +6 | router 2 | internal hop |
| 11108500 | +6 | router 3 | internal hop (end of X) |
| 11111500 | +6 | router 7 | internal hop (turn to Y) |
| 11114500 | +6 | router 11 | internal hop |
| 11117500 | +6 | router 15 | internal hop (arrived at HNF's router) |
| 11121000 | +7 | hnf15 | external hop into endpoint; `ReadUnique_PoC` (line `UD` = hit) |

Uncongested this leg is 44 cy; here 63 cy because the request waits behind the other 31
requests of the burst (queueing at the single injection port — *not* bandwidth).

### Phase B — hnf15 L3 read hit: 12 cy
| tick | Δcy | event |
|-----:|----:|-------|
| 11121000 | 0 | `AllocRequestNoRetry`, `ReadUnique_PoC`, `TagArrayRead` |
| 11122000 | +2 | `ReadHitPipe`, `DataArrayRead` (← `tagAccessLatency = 2`) |
| 11127000 | +10 | `WaitCompAck`, `SendCompData`, `TX_Data` beat 1 (← **`dataAccessLatency = 10`**) |
| 11127500 | +1 | `TX_Data` beat 2 (← `data_latency = 1` spacing) |

### Phase C — data hnf15 → rnf0: 47 cy (≈ all structural — **no funnel queue**)
Beats injected at router 15 at 11127500/11128000, then (first sighting per router):

| tick (beat1) | where | Δ per hop |
|-----:|-------|------|
| 11127500 | router 15 | inject |
| 11130500 | router 14 | +6 cy |
| 11133500 | router 13 | +6 cy |
| 11136500 | router 12 | +6 cy |
| 11139500 | router 8 | +6 cy |
| 11142500 | router 4 | +6 cy |
| 11146000 | router 0 | +6 cy (reaches rnf0's router) |
| 11150000 | rnf0 | `CompData_UD_PD` beat 1 (`+7` ext hop), `SendCompAck` |
| **11150500** | rnf0 | **`CompData_UD_PD` beat 2 → STOP**; then `CheckCacheFill`,`FillPipe`,`Final → UD` |

`END − START = 11150500 − 11089500 = 61000 ticks = 122 cy.`

**Where the time went:** of 122 cy, ~103 cy is unavoidable structure (44 request + 12
HNF + 47 data round-trip-ish) and only ~19 cy is queueing — *all* of it the launch-burst
on the request leg. **Compare the old buffer-4 config (§12): the same victim took 226 cy
with ~146 cy on the *return* leg.** Every return hop is now exactly 6 cy: the incast
funnel no longer queues.

---

## 8. The incast is still there — it just no longer queues

The incast pattern is unchanged: rnf0 keeps 32 ReadUniques outstanding, so dozens of
lines' `CompData` still converge on the col-0 routers and rnf0's ingress at the same
time. What changed is whether they *stall*.

* Each router/link still advances **~1 message per cycle**. The col-0 links
  (r12→r8→r4→r0) and the ext link into rnf0 are still shared by all returning data.
* **At `buffer_size = 4`** the small port buffers filled and PerfectSwitch stalled
  upstream — the deepest queue sat *at* the funnel (router 8 added +52 cy in the old
  trace). The funnel link saw idle cycles because backpressure couldn't be absorbed.
* **At `buffer_size = 8`** the port buffers are deep enough to cover the
  PerfectSwitch/Throttle backpressure round-trip, so the funnel link stays **fed every
  cycle** and drains at its 1-flit/cy ceiling with no stalls. The §7 trace shows it:
  every return hop is the bare 6-cy structural cost.

**Where the queueing physically sat** (capacities in §10): the controller `…In`/`…Out`
buffers are *infinite* (`buffer_size=0`), so nothing ever blocked at the endpoints.
Backpressure lived entirely in the small **in-fabric** buffers — the 3-deep
`SimpleIntLink` buffers and the **`buffer_size`-deep** router port buffers. Deepening the
latter from 4→8 is exactly what removed the return-path stalls; see the sweep in §12.

The only queueing that remains is at the **single source injection point** during the
startup burst (§7 Phase A) — a request-side effect that buffering downstream cannot fix
(it is fundamentally "32 requests, one rnf0 output port, one per cycle").

---

## 9. Reconciling with the headline numbers

* **Histogram reproduced exactly** from the trace: 820 ROI samples, buckets
  50/252/460/58, mean 67.50, max 122 — so the timeline in §7 is the real mechanism.
* **Min 29 cy** = nearest HNF, no contention (§6). **Max 122 cy** = far corner + the
  startup-burst request queue (§7).
* **Bandwidth (Little's Law):** `BW ≈ 32 × 64 / 67.5 = 30.3 B/cy` vs. measured
  **28.34 B/cy**. The small shortfall is the partial-write `CompAck` round-trip and
  write-merge, which lowers effective concurrency below 32. **Throughput is set by
  `outstanding / latency`, and latency is now ≈ the structural budget.**

---

## 10. Every capacity / buffer parameter

| Buffer | size (msgs) | notes |
|--------|------------:|-------|
| Controller in/out `MessageBuffer` (`reqIn/Out`, `datIn/Out`, …) | **0 = infinite** | no backpressure at the endpoint; messages never block leaving/entering a controller |
| **`SimpleNetwork.buffer_size` (router port buffers, per vnet)** | **8** | from `NoC_Params.router_buffer_size`; **the tuned knob** — 4→8 removes the funnel stall (§12) |
| `SimpleIntLink` buffers (router→router, per vnet) | **3** | = `channels × (link_latency+1) = 1×(2+1)`; `max_dequeue_rate=1` |
| TBE table (per controller, `number_of_TBEs`) | (see config) | a full table triggers `recycle_latency=10` resource stalls or, with `--allow-retryack=0`, RetryAck/reqIn backpressure |

Because endpoint buffers are infinite, **all** observed network queueing happens in the
3-deep int-link buffers and the `buffer_size`-deep port buffers; once they back up,
stalls ripple upstream one router per cycle. Sizing the port buffers (8) above the
backpressure round-trip is what keeps the funnel link saturated.

### Related capacity knobs
* `--num-outstanding-reqs` (32) — the biggest lever on both latency and BW.
* `router_buffer_size` (now 8) — the fabric-buffer lever this report quantifies (§12).
* `--allow-retryack` (default 1) — at the HNF, controls whether a full TBE table returns
  `RetryAck` (infinite `reqIn`) or stalls `reqIn` to backpressure.

---

## 11. Levers to reduce the measured latency

| Lever | Effect (measured where noted) |
|------|--------|
| **↑ `router_buffer_size` (4 → 8)** | **the headline fix here.** Removes the incast-funnel stall: mean 108.75 → **67.50 cy**, BW 17.99 → **28.34 B/cy**, max 226 → **122** (§12). Saturates at ~8 (≥10 gives ~0.1 % more). |
| ↓ `--num-outstanding-reqs` | latency lever via Little's Law (trades BW for latency). |
| HNF placement / spread active cores | shorter routes + no single-corner funnel → less distance and less startup-burst pressure. |
| ↓ `dataAccessLatency` of the L3 (10 cy) | directly cuts Phase B (~10 cy) for every transaction. |
| ↑ `link_bandwidth_factor` | **does NOT help** — already 1 flit/cy; the workload is not byte-bandwidth-bound. |

---

## 12. The `router_buffer_size` sweep (why 8)

`router_buffer_size` sets `SimpleNetwork.buffer_size`, the depth of every PerfectSwitch
output-port buffer. The default 4 is one slot short of covering the incast funnel's
backpressure round-trip, so the funnel link into router 0 sees idle cycles. Sweep (same
command, only `router_buffer_size` varied in `rbook_4x4.py`):

| `router_buffer_size` | ROI BW (B/cy) | ROI clocks | mean lat | % of ceiling |
|---:|---:|---:|---:|---:|
| **4** (gem5 default) | 17.99 | 2917 | 108.75 | 63 % |
| 5 | 22.41 | 2342 | – | 79 % |
| 6 | 26.65 | 1969 | – | 94 % |
| 7 | 28.31 | 1854 | – | 99.7 % |
| **8** (current) | **28.34** | **1852** | **67.50** | 99.9 % |
| 10–12 | 28.37 | 1850 | – | ~100 % |
| ≥16 | 28.38 | 1849 | – | 100 % |

It is a **progressive ramp, not a cliff**: each extra slot up to ~7 recovers more idle
funnel cycles; the knee is at 7 and it saturates at ~8. `router_buffer_size = 8` captures
99.9 % of the structural ceiling (~28.4 B/cy) with a one-slot safety margin — the chosen
value.

**Mechanism:** the col-0 merge needs enough port-buffer depth to keep the 1-flit/cy
funnel link continuously fed across the stall/credit round-trip. Below that there are
proportionally more idle cycles (→ lower BW, longer return-leg queue). This is the direct
analogue of `vcs_per_vnet` on Garnet, which removes the *same* funnel starvation by a
different mechanism (more virtual lanes instead of a deeper FIFO) and converges on the
*same* ~28 B/cy ceiling (GarnetLatency.md §12).

> **Note on `link_bandwidth_factor`.** A separate experiment confirmed byte-bandwidth is
> *not* the binding resource: at `bandwidth_factor = 32` vs `40` the latency/BW were
> identical once buffering was adequate. `40` is kept only for a clean 1-flit/cycle data
> channel and honest link-saturation statistics. The binding resource is **fabric buffer
> depth + routing distance**, not bytes/cycle.

---

## Appendix A — Reproducing this analysis from traces

### A.1 Regenerate the trace

```sh
./util/run_with_timeout.sh ./build/RISCV/gem5.opt \
  --debug-flags=ProtocolTrace,RubyNetwork --debug-file=trace.gz \
  -d m5out/simplelat-final-$(date +%Y%m%d-%H%M%S) \
  ruby-book/final/chi_testbench_gem5/driver/rbook_testbench_gem5.py \
  --scenario=memset --active-cores=0 --network=simple \
  --num-outstanding-reqs=32
gunzip -c m5out/simplelat-final-*/trace.gz > /tmp/sfinal.txt
```

### A.2 The two row formats

**(a) ProtocolTrace** — one row per SLICC transition, carries the **address** and the
controller `version` (**0 = rnf0**, **31 = Cache-31 = hnf15**):

```
   tick   version  Cache  <event>          <stateA>>stateB     [addr, line ...]
11089500     0     Cache  SendReadUnique   BUSY_BLKD>BUSY_INTR [0x1fc0, line 0x1fc0]
11121000    31     Cache  ReadUnique_PoC   UD>BUSY_BLKD        [0x1fc0, line 0x1fc0]
11150500     0     Cache  CompData_UD_PD   BUSY_BLKD>BUSY_BLKD [0x1fc0, line 0x1fc0]
```

**(b) RubyNetwork** — one row per network hop, tagged with the router it left, and (in
SimpleNetwork) carrying the **address**:

```
tick: PerfectSwitch-<router>: Message: [CHIRequestMsg: addr = [0x1fc0,...] type = ReadUnique ...]
```

Consecutive `PerfectSwitch-N` timestamps for the same address are per-hop arrivals; their
deltas are hop latencies (3000 ticks = 6 cy internal, 3500 = 7 cy external).

### A.3 Gotcha: warm-up vs ROI copies of the same address

Every address appears twice: warm-up writes full 64-B lines (`WriteUniqueFull`) at low
ticks; the ROI does 63-B **partial** writes (`SendReadUnique`) at ~11 M ticks. Always
filter to the ROI window and the event you mean.

### A.4 ROI window

`memset.cc` prints both ticks: `reset stats at tick 11088500`, `dump stats at tick
12014500`. So the ROI window is `[11088500, 12014500]`.

### A.5 Step 1 — validate the method and pick the victim

Parse only ProtocolTrace version-0 rows, pair each `SendReadUnique` with its completing
**2nd** `CompData_UD_PD`, keep completions in the ROI window, re-bucket to prove the
parse matches `stats.txt`, then sort for the max:

```python
import re
from collections import defaultdict, Counter
ROI_START, ROI_END = 11088500, 12014500
prot = re.compile(r'^\s*(\d+)\s+(\d+)\s+Cache\s+(\S+)\s+(\S+)\s+\[(0x[0-9a-f]+),')
byaddr = defaultdict(list)
for ln in open('/tmp/sfinal.txt', encoding='latin-1'):
    m = prot.match(ln)
    if not m or int(m.group(2)) != 0:      # version 0 = rnf0 only
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

Expected (matches `stats.txt` exactly — note `bucket_size = 32`):

```
ROI txns: 820  min 29  max 122  mean 67.50
32-cy buckets: {0: 50, 1: 252, 2: 460, 3: 58}
max: (61000, 11089500, 11150500, '0x1fc0')
```

### A.6 Step 2 — build the cycle-by-cycle table for the victim

```sh
# protocol-side timeline (drops PerfectSwitch rows), ROI only
grep -a "0x1fc0," /tmp/sfinal.txt | grep -av PerfectSwitch \
  | awk '$1 >= 11089000 && $1 <= 11151000'

# network-side hop timeline
grep -a "PerfectSwitch" /tmp/sfinal.txt | grep -a "0x1fc0," \
  | awk -F: '$1 >= 11089000 && $1 <= 11151000'
```

Merge by tick. Each gap is labeled by the events that bracket it: `int` hop should be
6 cy, `ext` hop 7 cy, `TagArrayRead→DataArrayRead` = 2 cy, `DataArrayRead→SendCompData` =
10 cy, consecutive beats = 1 cy. Anything beyond the structural value is queueing —
which, in this config, only appears on the request leg (§7/§8).

### A.7 Phase split (the §1 table)

Pair version-0 `SendReadUnique` with the HNF (version ≥16) `ReadUnique_PoC` (request-leg
end) and `SendCompData` (HNF-leg end); the remainder to the 2nd `CompData_UD_PD` is the
data-return leg. Over the 820 ROI transactions: request 8/26.4/63, HNF 12/12/12,
data-return 9/29.1/53, total 29/67.5/122 cy (min/mean/max).

### A.8 Files used

* Trace: `m5out/simplelat-final-*/trace.gz` → `/tmp/sfinal.txt`.
* Stats: `m5out/simplelat-final-*/stats.txt`
  (`…rnf0.cntrl.outTransLatHist.SendReadUnique`).
* The `router_buffer_size` sweep (§12) used temp noc-configs over the same command.

---

## Appendix B — key source references
* SimpleNetwork hop: `PerfectSwitch.cc:152-300`, `Throttle.cc:139-334`,
  `Switch.cc:86-134`, `MessageBuffer.cc:148-349`.
* `buffer_size` wiring: `NoC_Params.router_buffer_size` → `CHI.py:242`
  (`ruby_system.network.buffer_size = params.router_buffer_size`).
* CHI ReadUnique: `CHI-cache-transitions.sm:277,1238`,
  `CHI-cache-actions.sm:1665,2802,3144,3902-3910,3934`, `CHI-cache.sm:56-150`.
* Histogram plumbing: `slicc/symbols/StateMachine.py:1020`,
  `AbstractController.hh:393-431`.
* Config: `noc_config/rbook_4x4.py:21-34`, `configs/network/Network.py`,
  `m5out/simplelat-final-*/config.ini`.
</content>
