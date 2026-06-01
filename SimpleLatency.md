# SimpleNetwork Latency & Buffering — `memset` on the CHI testbench

A cycle-accurate dissection of where every clock cycle of a CHI `ReadUnique`
transaction comes from, for the run:

```
build/RISCV/gem5.opt -d m5out/rbook-tb-gem5-memset \
  ruby-book/final/chi_testbench_gem5/driver/rbook_testbench_gem5.py \
  --scenario=memset --active-cores=0 --network=simple \
  --simple-physical-channels --num-outstanding-reqs=32
```

Observed result we are explaining (current config, `link_bandwidth_factor = 40`):

```
memset ROI average BW: 17.991087 Bytes/Clock per core
system.ruby.rnf0.cntrl.outTransLatHist.SendReadUnique::mean   108.750000   (cycles)
  201 (0-63cy) | 360 (64-127) | 163 (128-191) | 96 (192-255) | 0 (256-319)
```

> **Config note.** `configs/example/noc_config/rbook_4x4.py` now sets
> `link_bandwidth_factor = 40` (= 32 B data flit + 8 B header → **one 256-bit data flit
> per cycle**). The earlier 32 B/cy setting gave an *identical* latency/BW (see §12 for
> the side-by-side proof that this knob does not move the needle on this workload); all
> numbers below are from the 40 B/cy config (runs in `m5out/simplelat-bw40-*`).

All numbers below were reproduced from a re-run with `--debug-flags=ProtocolTrace,RubyNetwork`.
A Python reconstruction of `outTransLatHist` from the `ProtocolTrace` reproduced the
stats histogram **exactly** (820 samples, buckets 201/360/163/96, mean 108.75 cy),
which validates the per-transaction timeline used in §7.

---

## 1. TL;DR — where the cycles go

`outTransLatHist.SendReadUnique` measures, **in requester (rnf0) clock cycles**, the
interval from *"the rnf cache injects the ReadUnique request"* to *"the last beat of
the line's data (CompData) has been received back"* (§5).

For this workload the latency splits into three additive parts:

| Phase | What happens | Min (cy) | Mean (cy) | Max (cy) |
|------|---------------|---------:|----------:|---------:|
| **A. Request out** | rnf0 → mesh → HNF | ~8 | ~26 | 44 |
| **B. HNF (L3) hit** | tag(2) + data-array(10) + inject(~2) | ~13 | ~13 | ~14 |
| **C. Data return** | HNF → mesh → rnf0 (**incast queueing lives here, §12**) | ~11 | ~70 | ~146 |
| **Total** | `outTransLatHist.SendReadUnique` | **33** | **108.75** | **226** |

Two things make the worst case ~7× the best case, and both are **network**, not protocol:

1. **Distance.** The home node (HNF) for a line is hash-interleaved across all 16
   mesh routers. The worst lines map to **router 15**, the corner diagonally
   opposite rnf0 (router 0) → a 6-hop X-Y route each way (§6).
2. **Incast queueing (NOT byte-bandwidth — see §12).** rnf0 keeps **32 requests
   outstanding**, so dozens of lines' worth of data responses are converging on the
   col-0 routers and rnf0's single ingress at the same time (measured in §8). They
   queue behind one another, and each router/link advances only **~1 message per
   cycle**. This adds ~100 cycles of queueing to the worst transaction (and, in the
   startup burst, the *request* also queues — §7). The limiting resource is
   **router/link message-slots per cycle plus routing distance**, *not* bytes/cycle:
   even with `link_bandwidth_factor = 40` (1 flit/cycle, zero link saturation) the
   latency is unchanged vs. the old 32 (§12).

The average bandwidth follows directly from the latency by Little's Law:
`BW ≈ N_outstanding × line_bytes / avg_latency = 32 × 64 / 108.75 ≈ 18.8 B/cy`,
matching the reported 17.99 B/cy (§9). **The workload is latency-bound, and the
latency is set by concurrency-driven queueing + distance — confirmed by experiment
in §12 (drop to 4 outstanding → mean latency 108.75 → 65.0 cy).**

---

## 2. System under test

* **Clock:** `system.clk_domain.clock = 500 ticks/cycle` → 2 GHz. Everything below is
  quoted in **cycles**; 1 cycle = 500 ticks. (`config.ini` line 46.)
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
  (partial line). Phases: L3 warm-up (1024 full-line stores) → ramp-up (102) →
  **ROI (820)** → ramp-down (102). Stats are reset at ROI start, so the histogram's
  820 samples == the 820 ROI writes.
* **Why `ReadUnique`:** a 63-byte (partial) store misses in the rnf leaf cache and must
  obtain the line in a writable (Unique) state, so the cache issues a CHI **ReadUnique**.
  Because the L3 was warmed, every ReadUnique **hits in some HNF's L3**, which returns
  `CompData_UD_PD`. (The full-line warm-up stores use `WriteUniqueFull` instead and are
  not in the ROI histogram.)
* **`--num-outstanding-reqs=32`:** the sequence keeps up to 32 ReadUniques in flight
  (`memset.cc:195`, `drv.outstanding() < depth`).

---

## 3. Every latency parameter (this run's values)

### 3a. CHI protocol / controller latencies (SLICC `CHI-cache.sm`, in **cycles**)

These are added when a controller *enqueues a message onto an output port* or when a
SLICC action sets `delayNextAction`. Defaults from `CHI-cache.sm:56-93`; values from
`config.ini`.

| Param | Value | Meaning / where it applies |
|-------|------:|----------------------------|
| `request_latency` | 1 | enqueue delay for a request msg (`Send_ReadUnique`, `reqOut`) |
| `response_latency` | 1 | enqueue delay for a response msg (`Comp*`, `rspOut`) |
| `data_latency` | 1 | enqueue delay **per data beat** (`Send_Data`, `datOut`) |
| `snoop_latency` | 1 | enqueue delay for a snoop msg |
| `mandatory_queue_latency` | 1 | seq request → mandatory queue |
| `allocation_latency` | 0 | TBE allocation |
| `read_hit_latency` / `read_miss_latency` | 0 / 0 | `ReadHitPipe`/`ReadMissPipe` pipe bubbles |
| `write_fe_latency` / `write_be_latency` | 0 / 0 | write front/back-end pipe bubbles |
| `fill_latency`, `snp_latency`, `snp_inv_latency`, `comp_*` | 0 | misc protocol bubbles |
| `recycle_latency` | 10 | **resource-stall retry** delay (TBE/queue full → re-process) |
| `to_memory_controller_latency` | 1 | HNF → SNF (memory). *Not used here: L3 hits.* |

The action that converts these to ticks is generated from SLICC `enqueue(port, Msg, N)`
→ `port.enqueue(out_msg, clockEdge(), cyclesToTicks(Cycles(N)), ...)`
(`src/mem/slicc/ast/EnqueueStatementAST.py:100`). Key actions:

* `Send_ReadUnique` (`CHI-cache-actions.sm:1665`) — `enqueue(reqOutPort, …, request_latency)`.
* `Send_CompData`/`Send_Data` (`…actions.sm:2802`, `:3144`) — `enqueue(datOutPort, …, data_latency)`, one beat at a time, `scheduleSendData(tbe, 1)` spaces beats by 1 cy.
* `Profile_OutgoingStart` / `Profile_OutgoingEnd_DataResp` (`…actions.sm:3902`, `:3905`) — the histogram's start/stop hooks (see §5).

### 3b. Cache-array access latencies (RubyCache SimObject — **separate** from §3a!)

The data-array access is the single biggest *protocol-side* cost and is easy to confuse
with `data_latency`. It comes from the `CacheMemory`/`RubyCache` object, applied by
`action(DataArrayRead)` → `dataLatency() = cache.getDataLatency()`
(`CHI-cache-funcs.sm:294`, `CHI-cache-actions.sm:3934`).

| Cache | `dataAccessLatency` | `tagAccessLatency` | size / assoc |
|-------|--------------------:|-------------------:|--------------|
| **hnf (L3)** `…hnf*.cntrl.cache` | **10 cy** | 2 cy | 16 MiB / 16-way |
| rnf (L2 leaf) `…rnf*.cntrl.cache` | 6 cy | 2 cy | 256 KiB / 8-way |

So an L3 read hit pays **tag 2 + data 10 = 12 cy** inside the HNF, independent of the network.

### 3c. SimpleNetwork latencies (`config.ini`, from `rbook_4x4.py:24-33`)

| Param | Value | Meaning |
|-------|------:|---------|
| `Switch.int_routing_latency` | 4 cy | PerfectSwitch routing latency when the **next** link is internal (router→router) |
| `Switch.ext_routing_latency` | 6 cy | PerfectSwitch routing latency when the next link is **external** (router→endpoint) |
| `SimpleIntLink.latency` | 2 cy | wire latency, router→router |
| `SimpleExtLink.latency` | 1 cy | wire latency, router↔endpoint |
| `link_bandwidth_factor` (`bandwidth_factor`) | **40** | **bytes/cycle** carried by each link → one 40 B data flit per cycle (see §4) |
| `endpoint_bandwidth` | 1000 | global bandwidth scale (with the ×1000 size multiplier) |
| `control_msg_size` | 8 B | wire size of a control message |
| `data_msg_size` | 32 B (+8 hdr = **40 B on the wire**) | wire size of one data beat |
| `number_of_virtual_networks` | 4 | req / rsp / snp / dat |
| `physical_vnets_channels` | `1 1 1 1` | from `--simple-physical-channels`: each vnet gets its own physical channel & bandwidth |

**Derived per-hop latencies** (verified directly in the trace, see §7):

* **Internal hop** (router→router) = `int_routing_latency + int_link_latency` = **4 + 2 = 6 cy**.
* **External hop** (router→endpoint) = `ext_routing_latency + ext_link_latency` = **6 + 1 = 7 cy**.
* **Injection** (endpoint→its own router) = just the controller enqueue latency
  (`request_latency`/`data_latency` = 1 cy), because the controller's `…Out` MessageBuffer
  *is* the PerfectSwitch input port.

---

## 4. How a single message hop works in SimpleNetwork

A message's journey for one hop touches three objects. Code paths:

```
controller.reqOut (MessageBuffer)            <- enqueue with request_latency
   │  (this buffer is the PerfectSwitch input port)
   ▼
PerfectSwitch::wakeup()  src/mem/ruby/network/simple/PerfectSwitch.cc:278
   │  moves head msg input→output port, adds routing_latency
   │  out_port.buffers[vnet]->enqueue(msg, now, out_port.latency)   (PerfectSwitch.cc:270)
   ▼
Switch port buffer (intermediate, size = network.buffer_size = 4)
   ▼
Throttle::wakeup()       src/mem/ruby/network/simple/Throttle.cc:248
   │  dequeues, applies BANDWIDTH (serialization), then
   │  out->enqueue(msg, now, cyclesToTicks(link_latency))            (Throttle.cc:203)
   ▼
next router input buffer (SimpleIntLink buffer, size 3)  OR  destination controller.reqIn
```

### 4a. PerfectSwitch = routing latency + arbitration
* Wakes when a message arrives; iterates vnets in priority order, round-robin over
  input ports, oldest-message-first (`PerfectSwitch.cc:152-183`).
* For each routable message it checks the destination port has space
  (`areNSlotsAvailable`, `PerfectSwitch.cc:216`); if **not**, it leaves the message in
  place and **reschedules itself +1 cycle** (`PerfectSwitch.cc:226`). *This is the
  backpressure that creates the queueing in §8.*
* On success it dequeues from the input and enqueues to the output port buffer with
  delay `out_port.latency` = `int_routing_latency` or `ext_routing_latency`
  (set in `Switch.cc:126-127`).
* `m_wakeups_wo_switch` (limit 128) periodically flips vnet priority to avoid starvation.

### 4b. Throttle = bandwidth (serialization) + wire latency
* `getLinkBandwidth(vnet) = endpoint_bandwidth × bandwidth_factor`
  = `1000 × 40 = 40000` "units"/cycle (`Throttle.cc:140-147`).
* `network_message_to_size(msg) = bytes × MESSAGE_SIZE_MULTIPLIER(1000)`
  (`Throttle.cc:322-334`). So **units == bytes × 1000**, and the link moves
  `40000/1000 = 40 bytes/cycle`.
* Each cycle the Throttle spends `min(units_remaining, bw_remaining)` (`Throttle.cc:224`).
  When a new message starts it is **immediately enqueued to the output with only
  `link_latency`** (Throttle.cc:203) — the serialization cost gates *when the next
  message on that link may move*, i.e. it models **throughput, not the head message's
  latency**. If bandwidth is exhausted the Throttle reschedules +1 cy (`Throttle.cc:304`).

  Confirmed in the trace (`throttle: … bw 40000 … enqueueing net msg N`):
  * **control** msg → `N = 8000` units = 8 B → drained in 1 cycle.
  * **data** beat → `N = 40000` units = 40 B → `40000` spent in **1 cycle** → exactly
    **one 256-bit data flit per cycle**, no leftover. (At the old `bandwidth_factor=32`
    this was 32000 + 8000 over **2 cycles/beat**; §12 shows that the extra cycle had no
    effect on end-to-end latency because this link is never the bottleneck.)

### 4c. `--simple-physical-channels`
Sets `physical_vnets_channels = [1,1,1,1]` (`configs/network/Network.py`), giving each
vnet its **own** physical channel and its **own** 40 B/cy budget (instead of 4 vnets
sharing one link's bandwidth). This is why request/response/data traffic on different
vnets don't serialize against each other — contention in §8 is purely *data-vnet vs.
data-vnet* (a queue for router/link **message-slots**, not for bytes).

---

## 5. What `outTransLatHist.SendReadUnique` actually measures

* **Defined** per `out_trans` event by SLICC codegen
  (`src/mem/slicc/symbols/StateMachine.py:1020`).
* **Start:** `action(Profile_OutgoingStart)` →
  `outgoingTransactionStart(addr, curTransitionEvent())`
  (`CHI-cache-actions.sm:3902`), fired in the requester transition
  `BUSY_BLKD --SendReadUnique--> BUSY_INTR` (`CHI-cache-transitions.sm:1238`).
  i.e. the instant rnf0 *commits to sending* the ReadUnique.
* **Stop:** `action(Profile_OutgoingEnd_DataResp)` →
  `outgoingTransactionEnd(...)` (`CHI-cache-actions.sm:3905`), which fires only once
  `tbe.expected_req_resp.hasReceivedData()` is true — i.e. **after the last expected
  data beat arrives**. The sample value is
  `ticksToCycles(curTick() − start)` (`AbstractController.hh:412-431`).
* A 64 B line / 32 B `data_channel_size` ⇒ **2 `CompData_UD_PD` beats**; the histogram
  stops on the **2nd** beat. (This is why version-0 shows 2 `CompData_UD_PD` per
  ReadUnique in the trace.)

So the metric spans: requester send → network out → HNF lookup → network back → both
data beats in. It does **not** include the earlier `Store`/`TagArrayRead` setup, nor
the trailing `CompAck`/`CheckCacheFill`/`Final` cleanup.

---

## 6. The uncongested latency budget (single transaction, no contention)

Per-hop costs from §3c, distance = Manhattan(router0, router_of_HNF).

```
A. Request out  = inject(1) + hops·6 + ext_in(7)
B. HNF L3 hit   = tag(2) + read_hit(0) + data_array(10) + send beats(~2)   ≈ 14
C. Data back    = inject(1) + hops·6 + ext_in(7)
```

* **Nearest HNF** (0–1 hops): ≈ 8 (A) + 13 (B) + 11 (C) = **~33 cy** → matches the
  measured ROI **minimum of 33 cy** (HNF on/near router 0).
* **Farthest HNF** (router 15, 6 hops): A = 1+36+7 = **44**, B ≈ 14,
  C uncongested = 44 → **~102 cy**.
* **Average** (uniform line→HNF hashing, mean distance ≈ 3 hops each way):
  ≈ 26 + 13 + 26 = **~65 cy uncongested** — and `--num-outstanding-reqs=4` measures
  **65.0 cy** (§12), i.e. with little concurrency the mean *is* the uncongested budget.

The measured **mean at 32 outstanding is 108.75 cy**, i.e. ~44 cy *above* the
uncongested average — that gap is queueing from concurrency. The structural
(uncongested) component explains the lower buckets; contention explains the long tail.

---

## 7. The maximum-latency transaction, cycle by cycle (226 cy)

Selected automatically as the ROI transaction with the largest
`outgoingEnd − outgoingStart`: **addr 0x1fc0, 226 cycles** (start tick 11172500,
end tick 11285500). HNF = `Cache-31` = **hnf15 on router 15** (the far corner). This is
the *first* ROI transaction — issued the instant stats reset, so it is caught in the
launch burst of all 32 outstanding requests, and queues on **both** legs. Times are from
`ProtocolTrace` (protocol events) and `RubyNetwork` (`PerfectSwitch-<router>` hops).

### Phase A — request rnf0 (router 0) → hnf15 (router 15): 67 cy (44 structural + ~23 burst queueing)
| tick | Δcy | where | event |
|-----:|----:|-------|-------|
| 11172000 | – | rnf0 | `Store I>BUSY_BLKD`, `TagArrayRead` (setup, *before* the clock) |
| **11172500** | 0 | rnf0 | **`SendReadUnique` → START** |
| 11183500 | +22 | router 0 | request leaves `reqOut` — **+22 cy queued** behind the 32-request launch burst on the shared X-path out of rnf0 |
| 11187500 | +8 | router 1 | hop = 6 cy route+link **+2 cy** queueing behind other requests |
| 11190500 | +6 | router 2 | internal hop |
| 11193500 | +6 | router 3 | internal hop (end of X-traversal) |
| 11196500 | +6 | router 7 | internal hop (turn to Y) |
| 11199500 | +6 | router 11 | internal hop |
| 11202500 | +6 | router 15 | internal hop (arrived at HNF's router) |
| 11206000 | +7 | hnf15 | external hop into endpoint (6 route + 1 link) |

Uncongested this leg is `1 + 6×6 + 7 = 44 cy`; here it is 67 cy because the request
itself waits behind the other 31 requests of the burst (queueing, not bandwidth).

### Phase B — hnf15 L3 read hit: 13 cy
| tick | Δcy | event |
|-----:|----:|-------|
| 11206000 | 0 | `AllocRequestNoRetry`, `ReadUnique_PoC`, `TagArrayRead` (line is `UD` in L3 = hit) |
| 11207000 | +2 | `ReadHitPipe`, `DataArrayRead` (← `tagAccessLatency = 2`) |
| 11212000 | +10 | `WaitCompAck`, `SendCompData`, `TX_Data` beat 1 (← **`dataAccessLatency = 10`**) |
| 11212500 | +1 | `TX_Data` beat 2 (← `data_latency = 1` spacing) |

The L3 **data-array access (10 cy)** dominates the protocol-side cost.

### Phase C — data hnf15 → rnf0: 146 cy (44 structural + ~102 queueing)
Beats injected at router 15 at 11212500/11213000, then (first sighting per router):

| tick (beat1) | where | note |
|-----:|-------|------|
| 11212500 | router 15 | inject |
| 11215500 | router 14 | +6 cy (normal) |
| 11218500 | router 13 | +6 cy (normal) |
| **11226500** | router 12 | **+16 cy** — queueing begins at the col-3→col-0 turn |
| **11252500** | router 8 | **+52 cy** — deep queue on the col-0 upward link (incast funnel) |
| 11263500 | router 4 | +22 cy |
| 11271500 | router 0 | +16 cy — reaches rnf0's router |
| 11275000 | rnf0 | `CompData_UD_PD` beat 1 received (`+7` ext hop), `SendCompAck` |
| **11285500** | rnf0 | **`CompData_UD_PD` beat 2 → STOP**; then `CheckCacheFill`,`FillPipe`,`Final → UD` |

`END − START = 11285500 − 11172500 = 113000 ticks = 226 cy.`

**Where the time went:** of 226 cy, only ~57 cy is unavoidable structure (44 routing
round-trip-ish + 13 HNF); the other ~125 cy is **queueing** split across the request
burst (~23 cy) and the return funnel (~102 cy). None of it is byte-bandwidth: each data
beat still occupies a link for just **1 cycle** here (§4b), yet the funnel queue is built
from *many beats sharing the same 1-message-per-cycle links*, not from byte serialization.

---

## 8. Why the return path queues — incast, quantified

During this transaction's return window (ticks 11212500–11285500), the distinct cache
lines whose `CompData` beats passed through **router 0** (rnf0's router) — i.e. all
responses competing for rnf0's ingress at that moment:

```
44 distinct lines' CompData converging on router 0 at the same time
```

A blocked message is re-examined by `PerfectSwitch` every cycle it cannot advance
(PerfectSwitch.cc:226), so the trace literally shows each beat sitting at routers 12/8/4
for many cycles — a direct readout of queueing time.

**The contention is for message-slots, not bytes:**
* rnf0 keeps 32 ReadUniques outstanding ⇒ dozens of lines of data in flight back to it.
* Each router/link advances **~1 message per cycle** (`SimpleIntLink.max_dequeue_rate=1`,
  PerfectSwitch moves one head message per port per cycle, and at `bandwidth_factor=40`
  one whole flit drains per cycle).
* The col-0 links (r12→r8→r4→r0) and the single ext link into rnf0 are **shared by all
  of them**, so ~44 lines × 2 beats funnelled through a 1-msg/cy pipe ≈ the ~100 cy of
  return queueing observed.
* Raising `bandwidth_factor` does **not** widen this pipe (it is already ≥1 flit/cy);
  only fewer outstanding requests, a shorter route, or not funnelling all traffic into
  one corner reduces it (§12).

**Where the queueing physically sits** (capacities from §10): the controller `…Out`/`…In`
buffers are *infinite* (`buffer_size=0`), so nothing blocks at the endpoints. Backpressure
instead fills the small **in-fabric** buffers — the 3-deep `SimpleIntLink` buffers and the
4-deep router port buffers — and PerfectSwitch/Throttle stall upstream routers once those
are full. That is exactly the r13→r12→r8→r4 stall chain seen in Phase C.

---

## 9. Reconciling with the headline numbers

* **Histogram reproduced exactly** from the trace: 820 ROI samples, buckets
  201/360/163/96, mean 108.75, max 226 — so the timeline in §7 is the real mechanism,
  not a model.
* **Min 33 cy** = nearest HNF, no contention (§6). **Max 226 cy** = far corner + incast (§7–8).
* **Bandwidth (Little's Law):** with `N=32` outstanding and 64 B/line,
  `BW ≈ N × 64 / mean_latency = 32 × 64 / 108.75 = 18.8 B/cy`, vs. measured
  **17.99 B/cy**. The small shortfall is because the partial-write transaction also needs
  the `CompAck` round and the write-merge, so the effective concurrency is slightly below
  32. **The takeaway: throughput is set by `outstanding / latency`, and latency is set by
  concurrency-driven incast queueing → the system is latency-bound, not bandwidth-bound
  (proven in §12).**

---

## 10. Every capacity / buffer parameter

| Buffer | size (msgs) | notes |
|--------|------------:|-------|
| Controller in/out `MessageBuffer` (`reqIn/Out`, `datIn/Out`, `rspIn/Out`, `snpIn/Out`) | **0 = infinite** | no backpressure at the endpoint; messages never block leaving/entering a controller |
| `SimpleNetwork.buffer_size` | 4 | default size of router/switch port buffers |
| Router **port buffers** (PerfectSwitch intermediate, per vnet) | 4 | `max_dequeue_rate=0` (unlimited/cycle) |
| `SimpleIntLink` buffers (router→router, per vnet) | **3** | = `channels × (link_latency+1) = 1×(2+1)`; `max_dequeue_rate=1` |
| TBE table (per controller, `number_of_TBEs`) | (see config) | a full table triggers `recycle_latency=10` resource stalls or, with `--allow-retryack=0`, RetryAck/reqIn backpressure |

Because endpoint buffers are infinite, **all** observed network queueing happens in the
3-deep int-link buffers and 4-deep port buffers; once they back up, stalls ripple
upstream one router per cycle.

### Related capacity knobs not exercised here
* `--num-outstanding-reqs` (32) — the single biggest lever on both latency and BW.
* `--allow-retryack` (default 1) — at the HNF, controls whether a full TBE table returns
  `RetryAck` (infinite `reqIn`) or stalls `reqIn` (size 2) to backpressure the network.
* `data_channel_size` (32) — beats per line (here 2 × 40 B). Raising it to 64 would make
  1 beat of 72 B wire → 2 cy serialization at 40 B/cy (vs 2×1 cy now); a throughput
  knob, irrelevant while the workload is latency-bound.

---

## 11. Levers to reduce the measured latency

| Lever | Effect (measured where noted, see §12) |
|------|--------|
| ↓ `--num-outstanding-reqs` | **biggest latency lever.** 32→4 outstanding: mean latency 108.75 → **65.0 cy** (but BW 17.99 → 3.76 — a latency/BW trade via Little's Law) |
| HNF placement / spread active cores | shorter routes + no single-corner funnel → less queueing & shorter distance |
| ↓ `dataAccessLatency` of the L3 (currently 10 cy) | directly cuts Phase B (~10 cy) for every transaction |
| ↑ `link_bandwidth_factor` (e.g. 32→40) | **does NOT help here.** Gives 1 flit/cycle and removes link saturation, but latency/BW unchanged because the workload is not byte-bandwidth-bound (§12) |
| Wider `data_channel_size` | fewer beats/headers; helps only once byte-bandwidth is actually the bottleneck (it isn't here) |

---

## 12. Why `link_bandwidth_factor = 40` (and the experiment behind the choice)

`data_channel_size=32` makes a data flit **40 B on the wire** (+8 B header). The config
now uses `link_bandwidth_factor = 40` so one 256-bit flit drains in exactly **1 cycle**
(verified in the trace: the data Throttle reports `bw 40000 … net msg 40000`, the whole
beat in a single cycle). The previous value, 32, needed 1.25 cy of bandwidth per flit
(drained over 2 cycles), which left the link's saturation stats lumpy and modelled a
slightly-narrower-than-intended data channel.

**Does the wider link improve performance? No — and that is the important finding.**
Comparison runs (same command; the only change is `link_bandwidth_factor` in
`rbook_4x4.py`, plus one run lowering `--num-outstanding-reqs`):

| Config | mean lat (cy) | max lat (cy) | ROI BW (B/cy) | rnf0-ingress bw-saturated cy |
|--------|--------------:|-------------:|--------------:|-----------------------------:|
| bw=32, 32 outstanding (old) | 108.76 | 227 | 17.99 | **1640** |
| **bw=40, 32 outstanding (current)** | **108.75** | 226 | **17.99** | **2** |
| bw=40, **4** outstanding | **65.0** | – | 3.76 | – |

So bw=40 did exactly what it should *mechanically* — 1 flit/cycle and the ingress data
link goes from "saturated" 1640/2917 ROI cycles to essentially never (2) — **but latency
and throughput did not move** (108.75 vs 108.76 cy; 17.99 vs 17.99 B/cy).

Why: byte-bandwidth was never the binding resource. At bw=32 the busiest link ran at
~70 % of its 25.6 B/cy useful ceiling; the `total_bw_sat_cy=1640` was an *artifact* of
the 40-vs-32 quantum (each beat's leftover 8 B exhausts the cycle), not a sustained
backlog. Latency is dominated by **routing distance + queueing among the 32 concurrent
transactions**, where every router/link advances ~1 message/cycle — independent of
bytes/cycle once you are at ≥1 flit/cycle. The `--num-outstanding-reqs` 4-vs-32 result
proves it: cutting concurrency cuts mean latency to **65.0 cy** (≈ the uncongested
structural budget of §6), while widening the link does nothing.

**Bottom line:** `link_bandwidth_factor = 40` is the right setting for a clean
1-flit-per-cycle data channel and honest link-saturation statistics (model fidelity) —
just don't expect it to speed up this latency-bound, single-core workload. (An earlier
draft of this report blamed `bw=32` for the latency; that was wrong and is corrected
here.)

---

## Appendix A — Reproducing this analysis from traces

Everything in §5–§9 (the histogram reconstruction, the max-latency victim, and the
cycle-by-cycle table in §7) is derived mechanically from one trace file. This appendix
documents the procedure end to end so the report is reproducible line-for-line.

### A.1 Regenerate the trace

One run with both debug flags writes an interleaved text trace. Per the repo rules, launch
through the timeout wrapper and into a timestamped output dir:

```sh
./util/run_with_timeout.sh ./build/RISCV/gem5.opt \
  --debug-flags=ProtocolTrace,RubyNetwork \
  --debug-file=trace.gz \
  -d m5out/simplelat-bw40-$(date +%Y%m%d-%H%M%S) \
  ruby-book/final/chi_testbench_gem5/driver/rbook_testbench_gem5.py \
  --scenario=memset --active-cores=0 \
  --network=simple --simple-physical-channels \
  --num-outstanding-reqs=32
```

`--debug-file=trace.gz` lands a gzip’d trace in the run dir. Decompress to a plain file
before grepping — macOS `grep` treats the file as binary if any NUL bytes survive, so use
`gunzip -c` then `grep -a` (treat as text):

```sh
gunzip -c m5out/simplelat-bw40-*/trace.gz > /tmp/bw40_trace.txt
```

### A.2 The two row formats

`ProtocolTrace,RubyNetwork` produces **two interleaved row shapes**; the entire method is
correlating them by address.

**(a) ProtocolTrace** — one row per SLICC controller state transition, plus `Seq`
begin/done that bracket the sequencer-visible latency:

```
   tick   version  Cache  <event>          <stateA>>stateB     [addr, line ...]
11171500     0     Seq    Begin            >                   [0x1fc0, line 0x1fc0] ST
11172500     0     Cache  SendReadUnique   BUSY_BLKD>BUSY_INTR [0x1fc0, line 0x1fc0]
11206000    31     Cache  ReadUnique_PoC   UD>BUSY_BLKD        [0x1fc0, line 0x1fc0]
11285500     0     Cache  CompData_UD_PD   BUSY_INTR>BUSY_BLKD [0x1fc0, line 0x1fc0]
```

`version` is the controller instance: **0 = rnf0**, **31 = Cache-31 = hnf15**. The tick is
absolute simulator ticks (500 ticks = 1 cycle @ 2 GHz).

**(b) RubyNetwork** — one row per network hop, tagged with the router it left:

```
tick: PerfectSwitch-<router>: Message: [CHIRequestMsg: addr = [0x1fc0,...] type = ReadUnique ...]
```

Consecutive `PerfectSwitch-N` timestamps for the same address are the per-hop arrivals;
their deltas are the hop latencies (3000 ticks = 6 cy internal hop, 3500 = 7 cy external),
and the router sequence is the literal route taken.

### A.3 Gotcha: warm-up vs ROI copies of the same address

Every address appears **twice** in the trace. The warm-up phase (`warmup_l3`) writes full
64-byte lines as `WriteUniqueFull`/`NCBWrData` at ticks ~10⁵; the ROI does 63-byte
**partial** writes on already-owned lines, which forces read-for-ownership and shows up as
`SendReadUnique` at ticks ~11.17 M. A naïve `grep "0x1fc0,"` returns the warm-up
`WriteUnique` lines first — always filter to the **ROI window** (below) and to the event
you mean.

### A.4 ROI window

The sequence resets stats at ROI entry and dumps at ROI exit; both ticks are printed to
stdout by `memset.cc` (`wait_for_start`/`wait_for_end`):

```
memset ROI: reset stats at tick 11171500
memset ROI: dump stats at tick 12630000
```

So the ROI window is `[11171500, 12630000]`. A transaction counts toward
`outTransLatHist.SendReadUnique` exactly when its completion lands in that window — the
same rule the C++ histogram uses after `statistics::reset()`.

### A.5 Step 1 — validate the method and pick the victim

`analyze.py` parses **only ProtocolTrace rows for version 0** (rnf0), pairs each
`SendReadUnique` (start) with its completing `CompData_UD_PD` (end = the 2nd/last data
beat), keeps transactions completing in the ROI window, and (a) re-buckets them into the
same 64-cycle buckets to **prove the parse matches `stats.txt`**, then (b) sorts to find
the max:

```python
import re
from collections import defaultdict
ROI_START, ROI_END = 11171500, 12630000
# tick  version  Cache  event  stateA>stateB  [addr,
prot = re.compile(r'^\s*(\d+)\s+(\d+)\s+Cache\s+(\S+)\s+(\S+)\s+\[(0x[0-9a-f]+),')

byaddr = defaultdict(list)
for ln in open('/tmp/bw40_trace.txt', encoding='latin-1'):
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
            results.append((after[1] - st, st, after[1], addr))   # ticks

results.sort(reverse=True)
lat = [r[0] // 500 for r in results]                              # ticks -> cycles
print("ROI txns: %d  min %d  max %d  mean %.2f" %
      (len(lat), min(lat), max(lat), sum(lat) / len(lat)))

from collections import Counter
buckets = Counter(min(c // 64, 4) for c in lat)                   # match 64-cy buckets
print("buckets:", dict(sorted(buckets.items())))
print("max:", results[0])                                        # (Δticks, start, end, addr)
```

Expected output for the bw=40 trace (matches `stats.txt` exactly):

```
ROI txns: 820  min 33  max 226  mean 108.75
buckets: {0: 201, 1: 360, 2: 163, 3: 96}
max: (113000, 11172500, 11285500, '0x1fc0')
```

The bucket vector `201/360/163/96` reproducing `outTransLatHist.SendReadUnique` is the
proof that the trace parse is faithful — only then is the §7 deep-dive trustworthy.

### A.6 Step 2 — build the cycle-by-cycle table for the victim

With the winning address (`0x1fc0`) and its window (`11172500 … 11285500`), pull **both**
views for that address in the ROI and read them in tick order:

```sh
# protocol-side timeline (drops PerfectSwitch rows), ROI only
grep -a "0x1fc0," /tmp/bw40_trace.txt | grep -av PerfectSwitch \
  | awk '$1 >= 11172000 && $1 <= 11286000'

# network-side hop timeline
grep -a "PerfectSwitch" /tmp/bw40_trace.txt | grep -a "0x1fc0," \
  | awk -F: '$1 >= 11172000 && $1 <= 11286000'
```

Merge the two by tick. **Every row in the §7 table is a tick-delta between two adjacent
events**, attributed to a category by which event pair brackets it:

| bracketing events | category | parameter it should equal |
|---|---|---|
| `Seq Begin → SendReadUnique` | injection / enqueue | `request_latency = 1 cy` |
| adjacent `PerfectSwitch-N` (internal) | NoC hop | `int_routing(4)+int_link(2) = 6 cy` |
| adjacent `PerfectSwitch-N` (endpoint) | NoC hop | `ext_routing(6)+ext_link(1) = 7 cy` |
| `TagArrayRead → DataArrayRead` | L3 tag access | `tagAccessLatency = 2 cy` |
| `DataArrayRead → SendCompData` | L3 data access | `dataAccessLatency = 10 cy` |
| consecutive data beats | beat spacing | `data_latency = 1 cy` |

The **structural minimum** for each gap is the parameter value above; anything measured
**beyond** it is queueing. That subtraction is exactly the "44 structural + ~N queueing"
split in §7, and the per-hop excess on the return path (e.g. router 8 at +52 cy vs. the
6-cy structural hop) is what §8 attributes to the incast funnel at router 0. Nothing in
§7/§8 is modeled — it is the literal tick column with each gap labeled by the events on
either side.

### A.7 Files used

* Trace: `m5out/simplelat-bw40-*/trace.gz` → `/tmp/bw40_trace.txt` (decompressed).
* Scripts: `/tmp/analyze.py` (full version + top-10 + validation),
  `/tmp/analyze2.py` (compact one-arg re-check).
* Stats cross-check: `m5out/simplelat-bw40-*/stats.txt`
  (`system.ruby.rnf0.cntrl.outTransLatHist.SendReadUnique`).
* The `--num-outstanding-reqs=4` control run (§12) uses the same procedure on
  `m5out/simplelat-oreq4-*/`.

---

## Appendix B — key source references
* SimpleNetwork hop: `PerfectSwitch.cc:152-300`, `Throttle.cc:139-334`,
  `Switch.cc:86-134`, `MessageBuffer.cc:148-349`, `Network.cc:58-66,166-200`.
* CHI ReadUnique: `CHI-cache-transitions.sm:277,1238,1477`,
  `CHI-cache-actions.sm:645,1665,2802,3144,3902-3910,3934`,
  `CHI-cache-funcs.sm:285-295`, `CHI-cache.sm:56-150`.
* Histogram plumbing: `slicc/symbols/StateMachine.py:1020`,
  `AbstractController.hh:393-431`.
* Config: `noc_config/rbook_4x4.py:21-53`, `configs/network/Network.py`,
  `m5out/rbook-tb-gem5-memset/config.ini`.
