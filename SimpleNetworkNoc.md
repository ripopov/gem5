# gem5 Ruby **SimpleNetwork** NoC — Complete Reference

> Scope: how `SimpleNetwork` (the non-Garnet Ruby interconnect) models
> **buffers**, **latency**, and **bandwidth**. Every claim is cited to source
> (`file:line`) and cross-checked against a generated `config.ini`. The running
> example is the 4×4 CHI mesh from
> `configs/example/noc_config/rbook_4x4.py`, launched as:
>
> ```sh
> ./build/RISCV/gem5.opt -d m5out/... \
>   ruby-book/final/chi_testbench_gem5/driver/rbook_testbench_gem5.py \
>   --scenario=memset --rn-mode=rnf_l2 --network=simple \
>   --num-outstanding-reqs=32 --simple-physical-channels --allow-retryack=0
> ```

---

## 1. The one-paragraph mental model

`SimpleNetwork` is **not** a flit/credit router model like Garnet. It is a set
of **`Switch`** objects connected by **`Link`** objects, where each switch is a
zero-contention crossbar (**`PerfectSwitch`**) followed by a per-output-port
**bandwidth limiter** (**`Throttle`**). Storage lives in **`MessageBuffer`**
objects. There is **no flit-level pipeline**; instead each *whole message* is
moved between buffers with a fixed delay, and a byte/cycle budget gates how many
messages can move per cycle. The two latencies that matter per hop are the
**routing latency** (PerfectSwitch → output buffer) and the **link latency**
(Throttle → next buffer). The bandwidth model only throttles *throughput*, it
does **not** add serialization latency to a single message (see §8, Pitfall P4).

---

## 2. Components and how they nest

```
SimpleNetwork  (src/mem/ruby/network/simple/SimpleNetwork.{py,cc,hh})
│
├── Switch[ ]            "routers" in config.ini  (one per mesh node)
│    │  (= BasicRouter subclass; Switch.cc / Switch.hh)
│    ├── PerfectSwitch          zero-contention crossbar (routing + arbitration)
│    ├── RoutingUnit            WeightBased XY routing (route table per out port)
│    ├── port_buffers[ ]        OUTPUT-side intermediate MessageBuffers
│    │                          (one per vnet per out port; SwitchPortBuffer)
│    └── Throttle[ ]            one per OUTPUT port; bandwidth + link latency
│
├── SimpleIntLink[ ]    switch→switch links; OWN their input MessageBuffers
│    └── buffers[ ]              one per vnet (this is the *input* queue of dst)
│
└── SimpleExtLink[ ]    switch↔controller links; carry NO buffer of their own
```

Key ownership facts (verified):

* A `Switch`'s only **own** buffers are the **output-side** `port_buffers`
  (`Switch.cc:65-68,118-124`, `SimpleNetwork.py:124`). They sit *between* the
  PerfectSwitch and the Throttle (`Throttle.hh:41-48`).
* A switch has **no dedicated input buffers**. Its input queue is whatever
  feeds the incoming link:
  * **Internal (switch→switch):** the buffer is owned by the `SimpleIntLink`
    (`SimpleLink.cc:65-71`, `SimpleNetwork.cc:151`).
  * **External (controller→switch):** the input *is the controller's own output
    queue* (`m_toNetQueues`), wired directly with no extra buffer
    (`SimpleNetwork.cc:138`).
* The Throttle's **output** buffer is the *next* stage's input: either the
  downstream `SimpleIntLink`'s buffer, or for the last hop the controller's
  receive queue `m_fromNetQueues` (`SimpleNetwork.cc:124-128,133`).

### 2.1 The three-stage switch (and why ordering matters)

```
        ┌──────────────────────── Switch ────────────────────────┐
 in →   │  PerfectSwitch ──(+routing_latency)──▶ port_buffer ──▶ Throttle ──(+link_latency)──▶ out
buffer  │  • route() via RoutingUnit             (SwitchPort-     • bandwidth budget
(int-   │  • check ALL out slots free            Buffer,          • whole-msg move
 link   │  • dequeue 1 msg, enqueue to            size=buffer_    • reschedule if
 or     │    each out port_buffer                 size)             bw-saturated/blocked
 ctrl)  │                                                          │
        └──────────────────────────────────────────────────────────┘
```

* PerfectSwitch runs at event priority `Default_Pri`; Throttle at
  `Default_Pri + 1` (`Switch.hh:85-86`). So within one tick the PerfectSwitch
  forwards *first*, then the Throttle drains — "the throttle sends messages to
  the links after the switch is done forwarding the messages in the same cycle"
  (`Switch.hh:83-86`). The header itself flags this as an *unrealistic*
  artifact of single-state modeling (`Switch.hh:43-51`).

---

## 3. rbook_4x4 topology (the concrete reference)

`--rn-mode=rnf_l2` RN nodes are **not** `CHI_RNF` instances, so
`_createRNFRouter` is **not** called — there are **no node/mux routers** in this
testbench. Result: **16 mesh routers**, controllers attach directly via ext
links (verified: `config.ini` has exactly `routers00..routers15`).

```
 router id (row-major)          controllers attached (router_list)
 0 --- 1 --- 2 --- 3            HNF: every router 0..15
 |     |     |     |            RNF: every router 0..15 (Seq→L2 leaf cache)
 4 --- 5 --- 6 --- 7            MN : router 0
 |     |     |     |            SNF_MainMem (DDR): routers 0 and 15
 8 --- 9 ---10 ---11
 |     |     |     |            XY routing weights: E/W=1, N/S=2
12 ---13 ---14 ---15            (CustomMesh.py:74  link_weights=[1,1,2,2])
```

Mesh links are bidirectional pairs created in `CustomMesh._makeMesh`
(`CustomMesh.py:60-184`); ext links in `distributeNodes`
(`CustomMesh.py:224-282`).

4 virtual networks (`CHI.py:236-237`): **vnet0=request, vnet1=snoop,
vnet2=response, vnet3=data**.

---

## 4. Complete BUFFER inventory

Every buffer in the datapath is a `MessageBuffer`
(`src/mem/ruby/network/MessageBuffer.{py,cc}`). A buffer holds whole messages in
a priority heap ordered by ready-time; `buffer_size = 0` means **infinite**
(`MessageBuffer.cc:151-154`).

| Buffer | Owner / created where | Per | `buffer_size` (rbook_4x4) | `ordered` | `max_dequeue_rate` | `allow_zero_latency` | Notes |
|---|---|---|---|---|---|---|---|
| **port_buffers** (intermediate, output side) | `Switch` via `setup_buffers` → `SwitchPortBuffer` (`SimpleNetwork.py:130-167`) | vnet × out-port | **4** (`= network.buffer_size × channels = 4×1`) | **true** | **0** (unlimited) | **true** | Sits between PerfectSwitch and Throttle. `buffer_size` comes from `router_buffer_size` (§7). |
| **int-link buffers** (switch *input* on internal links) | `SimpleIntLink` via `setup_buffers` (`SimpleLink.py:67-99`) | vnet | **3** (`= channels × (latency+1) = 1×(2+1)`) | **true** | **1** (`= channels`) | **false** | Only sized this way **because** `physical_vnets_channels` is set; otherwise infinite + rate 0 (Pitfall P5). |
| **ext-in queue** (controller→switch) | `m_toNetQueues`, the controller's own send queue (`SimpleNetwork.cc:138`) | vnet | controller-defined | per controller | per controller | per controller | Network does not own/size it; it's the producing controller's output `MessageBuffer`. |
| **ext-out / dest queue** (switch→controller) | `m_fromNetQueues`, the controller's receive queue (`SimpleNetwork.cc:122-128`) | vnet | controller-defined | per controller | per controller | per controller | The Throttle's final `out`. |

Verified from `config.ini`:
* `routers00.port_buffers00`: `buffer_size=4, max_dequeue_rate=0, ordered=true,
  allow_zero_latency=true, randomization=ruby_system`. Router 00 has **24**
  port_buffers = 6 out-ports × 4 vnets.
* `int_links00.buffers0`: `buffer_size=3, max_dequeue_rate=1, ordered=true,
  allow_zero_latency=false`.

### 4.1 Buffer capacity = backpressure (credits)

`buffer_size > 0` makes a buffer a finite, credit-like resource:

* **PerfectSwitch** will not move a message unless **every** destination
  `port_buffer` has a free slot — it checks `areNSlotsAvailable(1)` for all
  output links and otherwise reschedules +1 cycle and head-of-line-stalls that
  input (`PerfectSwitch.cc:210-231`).
* **Throttle** will not move a message unless its `out` buffer has a slot;
  otherwise it sets `output_blocked` and retries next cycle
  (`Throttle.cc:185-186,237-244,300-305`).

So the flow-control chain for one hop is:

```
ctrl in-q  ◀──Throttle◀── port_buffer(4) ◀──PerfectSwitch◀── int-link buf(3) ◀──upstream Throttle ◀── ...
   full?        stalls        full?            stalls            full?              stalls
```

A full downstream buffer propagates *backwards* and eventually stalls the
producing controller. (This is exactly the mechanism `--allow-retryack=0`
relies on: it shrinks HNF `reqIn` to 2 so backpressure reaches the network,
`rbook_testbench_gem5.py:311-314`.)

---

## 5. Complete LATENCY inventory

There are exactly **two** configurable per-hop latencies, plus a few implicit
ones. All are applied as the `delta` (in ticks) when a message is enqueued into
the *next* buffer: `arrival_time = current_time + delta`
(`MessageBuffer.cc:236-244`); the message cannot be dequeued before
`arrival_time` (`MessageBuffer.cc:540-541`).

| Latency | Param | Applied where | Direction it depends on | rbook_4x4 value |
|---|---|---|---|---|
| **Routing latency** | `Switch.int_routing_latency` / `ext_routing_latency` (`SimpleNetwork.py:112-117`) | PerfectSwitch enqueue → `port_buffer` (stored as `OutputPort.latency`, `PerfectSwitch.cc:134,270-272`); chosen in `Switch.cc:126-127` | by **outgoing** link type: internal hop → `int_`, hop to a controller → `ext_` | **int=4, ext=6** |
| **Link latency** | `Link.latency` = `BasicLink.latency` (`BasicLink.py:39`, `BasicLink.cc:40`) | Throttle enqueue → next buffer (`Throttle.cc:203-206`) | per link | int link **2**, ext link **1** |
| **`router_latency`** | `BasicRouter.latency` (`BasicRouter.py:39`) | *Only* the default source for `int/ext_routing_latency`. **Not used directly** by SimpleNetwork's datapath once `int/ext_routing_latency` are set. | — | 4 (informational) |
| **`max_dequeue_rate` stall** | `MessageBuffer.max_dequeue_rate` (`MessageBuffer.py:77-82`) | Caps dequeues/cycle from a buffer; extra msgs wait to next cycle (`MessageBuffer.cc:537-546`) | per buffer | int-link bufs = **1/cycle/vnet**; others unlimited |
| **Randomization delay** | RubySystem `randomization` + buffer `randomization` (`MessageBuffer.py:48-49,63-65`) | If enabled, **`delta` is ignored** and a random delay is used instead (`MessageBuffer.cc:240-255`) | global/per-buffer | **disabled** here (`config.ini randomization=false`) → see Pitfall P1 |

### 5.1 `router_latency` vs routing latency — the classic trap

`int_routing_latency`/`ext_routing_latency` default to `BasicRouter.latency`,
but that default binds to the **Param's literal default value (1)** at class
definition time, *not* to the per-router `latency` you set in a topology
(`SimpleNetwork.py:112-117`). So setting only `router_latency` leaves the routing
latencies at 1 unless you pass them explicitly. In rbook_4x4 they are wired
through `CustomMesh` from the noc_config (`CustomMesh.py:305-314`,
`rbook_4x4.py:32-33`), which is why `config.ini` shows `latency=4,
int_routing_latency=4, ext_routing_latency=6` on each mesh router.

### 5.2 Per-hop and end-to-end latency (no contention, no randomization)

Per single message moving across one output port:

```
hop_latency = routing_latency(out-port type) + link_latency(that link)
```

For rbook_4x4:

| Hop type | routing | link | **total** |
|---|---|---|---|
| switch → switch (mesh) | int = 4 | int link = 2 | **6 cycles** |
| switch → controller (final delivery) | ext = 6 | ext link = 1 | **7 cycles** |
| controller → switch (injection) | (none on input) | — | first routing happens at the switch's chosen out port |

End-to-end for a message from controller A (router `a`) to controller B
(router `b`) over `H` mesh hops:

```
latency ≈ Σ_(mesh hops) (int_routing + int_link)  +  (ext_routing + ext_link)
        = H × 6  +  7      (rbook_4x4, contention-free)
        + queueing/backpressure + (randomization if enabled)
```

Example: RNF at router 5 → HNF at router 6 (1 mesh hop east, then ext deliver):
`1×6 + 7 = 13` cycles minimum. (XY routing, `WeightBased::findRoute`,
`WeightBased.cc:106-127`.)

---

## 6. Routing (WeightBased)

* Default routing unit is `WeightBased(adaptive_routing=False)`
  (`SimpleNetwork.py:126-128`).
* `findRoute` walks output ports in **weight order** and, for each, sends the
  message to that port if the destination set intersects the port's routing
  table; it splits the destination set across ports for multicast
  (`WeightBased.cc:106-127`). Lower `weight` = preferred → with `[E/W=1,
  N/S=2]` this yields **X-before-Y** deterministic routing
  (`CustomMesh.py:74`).
* `adaptive_routing` (off here) would reorder links by output-queue occupancy
  for non-ordered vnets (`WeightBased.cc:79-101`).

---

## 7. BANDWIDTH model — the part that surprises people

### 7.1 The unit system

* Each message has a `MessageSizeType`; `MessageSizeType_to_int` maps it to a
  byte size (`Network.cc:165-192`):
  * control-class → `control_msg_size`
  * data-class → `data_msg_size`
* `network_message_to_size` multiplies that by `MESSAGE_SIZE_MULTIPLIER = 1000`
  (`Throttle.cc:60,322-334`). So a message "costs" `bytes × 1000` **units**.
* A link's budget per cycle is `endpoint_bandwidth × bandwidth_factor`
  **units/cycle** (`Throttle.cc:139-147`; `endpoint_bandwidth` from
  `SimpleNetwork.endpoint_bandwidth`, `bandwidth_factor` from the link's
  `bandwidth_factor` → `m_bw_multiplier`, `SimpleLink.cc:56,67`).

Because `endpoint_bandwidth = 1000 = MESSAGE_SIZE_MULTIPLIER`:

```
bytes_per_cycle  =  endpoint_bandwidth × bandwidth_factor / 1000
                 =  bandwidth_factor            (when endpoint_bandwidth=1000)
```

So `link_bandwidth_factor = 32` ⇒ **32 bytes/cycle per link/channel**. (If you
ever change `endpoint_bandwidth`, this identity breaks — Pitfall P3.)

### 7.2 Message sizes in rbook_4x4 (verified)

| Class | Source | Bytes |
|---|---|---|
| control | `control_msg_size = cntrl_msg_size` (`CHI.py:239`, `CHI_config.py:114`) | **8** |
| data | `data_msg_size = data_width` (`CHI.py:240`) **plus control** added internally (`Network.cc:66`) | **32 + 8 = 40** |

`config.ini` shows `control_msg_size=8, data_msg_size=32`; the `+8` happens in
C++ (`Network.cc:66`), so the **effective data network message is 40 bytes**
(Pitfall P2).

### 7.3 How many cycles a message occupies the link

The Throttle spends `min(units_remaining, bw_remaining)` per cycle and
reschedules itself while saturated (`Throttle.cc:223-228,300-305`):

| Message | units | cycles to fully "pay" at 32000 units/cy |
|---|---|---|
| control (8 B) | 8 000 | 1 |
| data (40 B) | 40 000 | ⌈40000/32000⌉ = **2** |

**Crucial:** these cycles are *occupancy/throughput*, not added latency to the
message itself — see Pitfall P4.

### 7.4 Per-vnet vs shared bandwidth — what `--simple-physical-channels` does

This is the single biggest behavioral switch in SimpleNetwork.

* `--simple-physical-channels` sets `physical_vnets_channels = [1,1,1,1]`
  (`configs/network/Network.py:277-282`).
* **With** physical channels (this run): each (vnet, channel) gets its **own**
  independent budget `getLinkBandwidth(vnet)` (`Throttle.cc:180-181,267-284`).
  Total link bandwidth = **Σ over vnets** (`Throttle.cc:149-158`). With 4 vnets
  → **4 independent 32 B/cy channels** (128 B/cy aggregate), and int-link input
  buffers become finite (size 3) with `max_dequeue_rate=1`
  (`SimpleLink.py:87-97`).
* **Without** physical channels (default): a **single shared** budget
  `total_bw_remaining` is split across all vnets in one `wakeup`
  (`Throttle.cc:180-181,252,268-284`) → one 32 B/cy pipe shared by all 4 vnets,
  and int-link buffers are **infinite** with no per-cycle dequeue cap.

```
WITHOUT --simple-physical-channels        WITH --simple-physical-channels
  one Throttle budget = 32 B/cy             per-vnet budget = 32 B/cy each
  shared by vnets 0..3                      vnet0:32  vnet1:32  vnet2:32  vnet3:32
  int-link bufs: INFINITE, rate ∞           int-link bufs: size 3, rate 1/cy
```

---

## 8. End-to-end datapath with cycle accounting

A request from RNF(router 5) to HNF(router 6), then HNF data response back.
Contention-free, randomization off, rbook_4x4 numbers.

```
RNF L2 cntrl @ r5
   │ enqueue to its toNetQueue (vnet0=req)        (controller-side latency)
   ▼
[Switch r5] PerfectSwitch.route → out-port = East(internal)
   │ +int_routing_latency (4)         ──▶ port_buffer (size 4)
   │ Throttle: control=8B → 1 cy budget; enqueue +int_link_latency (2)
   ▼                                              hop cost = 4+2 = 6 cy
[int-link r5→r6 buffer] (size 3, dequeue ≤1/cy)
   ▼
[Switch r6] PerfectSwitch.route → out-port = ext link to HNF
   │ +ext_routing_latency (6)         ──▶ port_buffer
   │ Throttle: enqueue +ext_link_latency (1)
   ▼                                              hop cost = 6+1 = 7 cy
HNF cntrl @ r6 receive queue (m_fromNetQueues)    REQUEST total ≈ 13 cy
```

The data reply (40 B) follows the reverse path; its latency is **also** ≈13 cy
contention-free — the extra bandwidth cycle for 40 B affects only the *next*
message's start, not this message's arrival (Pitfall P4).

---

## 9. Parameter reference tables

### 9.1 Network-level (`SimpleNetwork`)

| Param | Default | rbook_4x4 | Where set | Effect |
|---|---|---|---|---|
| `buffer_size` | 0 = infinite (`SimpleNetwork.py:53-57`) | **4** | `CHI.py:242` ← `router_buffer_size` | Size of every `port_buffer` (and the multiplier base for int-link bufs). |
| `endpoint_bandwidth` | 1000 (`SimpleNetwork.py:58`) | 1000 | default | BW scaling; per-cycle units = `endpoint_bandwidth × bandwidth_factor`. |
| `physical_vnets_channels` | `[]` (`SimpleNetwork.py:60-64`) | `[1,1,1,1]` | `configs/network/Network.py:278-281` (`--simple-physical-channels`) | Per-vnet independent BW + finite/rate-limited int-link bufs. |
| `physical_vnets_bandwidth` | `[]` (`SimpleNetwork.py:66-71`) | `[]` | — | Per-vnet BW override; needs `physical_vnets_channels`. |
| `number_of_virtual_networks` | (Network.py) | **4** | `CHI.py:237` | req/snoop/resp/data. |
| `control_msg_size` | 8 (`Network.py:54`) | 8 | `CHI.py:239` | Control message bytes. |
| `data_msg_size` | (`Network.py:65`) | 32 → **40 eff.** | `CHI.py:240` (+8 in `Network.cc:66`) | Data message bytes. |

### 9.2 Switch / router (`Switch` : `BasicRouter`)

| Param | Default | rbook_4x4 | Where | Effect |
|---|---|---|---|---|
| `latency` | 1 (`BasicRouter.py:39`) | 4 | `CustomMesh.py:384-391` ← `router_latency` | Default for routing latencies; not otherwise used by datapath. |
| `int_routing_latency` | `BasicRouter.latency` literal = 1 (`SimpleNetwork.py:112-114`) | **4** | `CustomMesh.py:305-309` ← noc_config | Delay PerfectSwitch→port_buffer for **internal** out ports. |
| `ext_routing_latency` | same = 1 (`SimpleNetwork.py:115-117`) | **6** | `CustomMesh.py:310-314` ← noc_config | Same, for **external** (to-controller) out ports. |
| `virt_nets` | `Parent.number_of_virtual_networks` | 4 | — | # vnets the PerfectSwitch iterates. |
| `routing_unit` | `WeightBased(adaptive=False)` (`SimpleNetwork.py:126-128`) | same | — | Routing policy. |
| `port_buffers` | auto (`SimpleNetwork.py:130-167`) | size 4 each | auto | Output intermediate buffers. **Do not set manually.** |

### 9.3 Links (`SimpleIntLink`/`SimpleExtLink` : `BasicLink`)

| Param | Default | rbook_4x4 | Where | Effect |
|---|---|---|---|---|
| `latency` | 1 (`BasicLink.py:39`) | int **2**, ext **1** | `CustomMesh.py:205,216,255,277` ← `router_link_latency`/`node_link_latency` | Throttle→next-buffer delay. |
| `bandwidth_factor` | ext 16 / int 16 (`BasicLink.py:59,75`) | **32** | `CustomMesh.py` ← `link_bandwidth_factor` | BW multiplier → bytes/cycle. |
| `weight` | 1 (`BasicLink.py:48`) | E/W 1, N/S 2 | `CustomMesh.py:74` | Routing preference. |
| `buffers` (int only) | auto (`SimpleLink.py:67-99`) | size 3, rate 1 | auto | Switch input queue (internal). |

### 9.4 The noc_config knobs (`rbook_4x4.py` `NoC_Params`)

| Knob | Value | Drives |
|---|---|---|
| `num_rows`,`num_cols` | 4,4 | 16 mesh routers |
| `router_latency` | 4 | router `latency` (and routing-latency default) |
| `router_link_latency` | 2 | internal mesh-link `latency` |
| `node_link_latency` | 1 (CHI default) | external link `latency` |
| `node_router_latency` | 2 | node/mux router latency (unused — no node routers here) |
| `link_bandwidth_factor` | 32 | every link `bandwidth_factor` (→ 32 B/cy) |
| `int_routing_latency` | `=router_latency` (4) | internal routing latency |
| `ext_routing_latency` | `=router_latency+2` (6) | external routing latency |
| `router_buffer_size` | 4 (CHI default) | `network.buffer_size` |

---

## 10. Hidden pitfalls & gotchas

* **P1 — Randomization nukes all configured latencies.** If the RubySystem
  `randomization` flag is on (and a buffer's `randomization` is `enabled` or
  `ruby_system`), the enqueue **ignores `delta`** entirely and uses
  `random_time()` = `1 + [0..3]` ticks, with a 1-in-8 chance of an extra
  `100 + [1..15]` (`MessageBuffer.cc:206-216,240-255`). Your
  `int/ext_routing_latency` and link latencies become irrelevant. Verify
  `config.ini` shows `randomization=false` (it does in this run).

* **P2 — Data messages are bigger than `data_msg_size`.** The config value
  (32) has `control_msg_size` (8) **added in C++** (`Network.cc:66`), so data
  messages cost **40 bytes**, not 32, for bandwidth. `config.ini` still prints
  `data_msg_size=32`.

* **P3 — `bytes/cycle == bandwidth_factor` only because `endpoint_bandwidth ==
  1000`.** The true formula is `endpoint_bandwidth × bandwidth_factor / 1000`
  (`Throttle.cc:60,139-147`). Change `endpoint_bandwidth` and the comment
  "`# SimpleNetwork bytes/cycle per link`" in the noc_config becomes wrong.

* **P4 — Bandwidth limits throughput, NOT single-message latency.** When the
  Throttle first services a message it **dequeues and enqueues the whole
  message immediately** with `delta = link_latency`
  (`Throttle.cc:201-206`); the leftover `units_remaining` only blocks the
  *next* message on that vnet/channel (`Throttle.cc:223-228`). So a 40-byte
  data message arrives at the next buffer after exactly `link_latency` ticks —
  the "2-cycle" cost delays the following message, it does not lengthen this
  one. SimpleNetwork has **no per-message serialization latency**.

* **P5 — `--simple-physical-channels` changes BW *and* buffering semantics, not
  just channel count.** It (a) gives each vnet an **independent** BW budget
  (aggregate ×#vnets vs a single shared pipe), and (b) makes internal-link
  input buffers **finite** (`channels×(latency+1) = 3`) with
  `max_dequeue_rate=channels=1`, where they'd otherwise be **infinite** and
  unthrottled (`SimpleLink.py:87-97`, `Throttle.cc:160-181`). This dramatically
  changes both bandwidth and backpressure. (Pitfall: comparing runs with/without
  this flag is not apples-to-apples.)

* **P6 — Routing latency is chosen by the OUTGOING link, not the incoming
  one.** A message entering from a controller and routed onward over a mesh link
  pays `int_routing_latency`; one routed to a controller pays
  `ext_routing_latency` (`Switch.cc:126-127`). There is **no** routing latency
  applied on the input side.

* **P7 — `router_latency` alone does nothing to routing delay.** Because the
  routing-latency Params capture `BasicRouter.latency`'s *literal default (1)*
  (`SimpleNetwork.py:112-117`), you must set `int/ext_routing_latency`
  explicitly (rbook_4x4 does, via `CustomMesh.py:305-314`). Otherwise routing is
  1 cycle regardless of `router_latency`.

* **P8 — Switches have no input buffering of their own** (§2). Backpressure and
  occupancy on the *input* side live on the links (internal) or on the
  producing controller's queue (external). Don't look for an input buffer on the
  `Switch` — it isn't there.

* **P9 — `port_buffers` are output-side, `allow_zero_latency=true`.** They
  permit a 0-tick routing latency (`SimpleNetwork.py:96-100`). Other buffers set
  `allow_zero_latency=false`, so a 0 `delta` there **panics**
  (`MessageBuffer.cc:234-235`).

* **P10 — `max_dequeue_rate` is a throughput cap, applied lazily.** A buffer at
  its per-cycle dequeue limit reports "not ready" and self-reschedules to next
  cycle (`MessageBuffer.cc:537-546`); the message is delayed even though it has
  met its arrival time. Only the int-link buffers have this here (rate 1).

* **P11 — Event-priority ordering is a known modeling artifact.** PerfectSwitch
  before Throttle in the same tick (`Switch.hh:43-51,85-86`) means timing can
  depend on wake order; the source itself calls this "un-realistic modelling."

* **P12 — Head-of-line blocking per (input-port, vnet); no skip-ahead.** The
  PerfectSwitch peeks **only the head** of an input buffer
  (`PerfectSwitch.cc:200`). If the head's required output `port_buffer` is full
  it reschedules +1 cycle and `break`s to the *next input port*
  (`PerfectSwitch.cc:210-231`) — it does **not** pick a younger message behind
  the head that wants a free output. Buffers are `ordered=true`
  (`SimpleNetwork.py:99`), so there is no reordering within a buffer. Example: a
  head destined South (South full) stalls a same-vnet, same-port message
  destined West, even though West is free. Blocking is **not** total, because
  each vnet has its own buffer/`operateVnet` (`PerfectSwitch.cc:152-183`) and a
  stall only `break`s to the next *input port* (`:230`) — so other vnets and
  other input ports keep flowing. This is virtual-channel-free, single-queue
  routing: expect HOL effects under load that a true VC router would avoid.

---

## 11. Source map (every claim, by file)

| Topic | File:line |
|---|---|
| Switch = PerfectSwitch + Throttles + port_buffers | `Switch.cc:57-69,85-134`; `Switch.hh:114-130` |
| Routing latency int/ext select | `Switch.cc:126-127`; params `SimpleNetwork.py:112-117` |
| PerfectSwitch route/resource-check/forward | `PerfectSwitch.cc:152-275`; routing latency stored as out.latency `:134,270-272` |
| Head-of-line blocking (peek head, break to next port) | `PerfectSwitch.cc:152-183,200,210-231`; ordered bufs `SimpleNetwork.py:99` |
| No input buffers on switch | `PerfectSwitch.cc:80-94`; `SimpleNetwork.cc:138,151` |
| Throttle bandwidth + link latency | `Throttle.cc:60,139-164,166-245,247-306,322-334` |
| Whole-message move (no serialization latency) | `Throttle.cc:201-206,223-228` |
| Per-vnet vs shared BW | `Throttle.cc:149-158,180-181,267-284` |
| MessageBuffer capacity / slots | `MessageBuffer.cc:147-192` |
| MessageBuffer arrival/delta/randomization | `MessageBuffer.cc:206-216,218-311` |
| MessageBuffer dequeue-rate / isReady | `MessageBuffer.cc:533-547` |
| Link latency/bw params | `BasicLink.py:39,47,48`; `BasicLink.cc:37-44`; `SimpleLink.cc:56,67` |
| int-link buffer sizing (physical) | `SimpleLink.py:67-99` |
| port_buffer sizing | `SimpleNetwork.py:130-167` |
| Message sizes / `+control` | `Network.cc:64-66,165-192`; `Network.py:54,65` |
| Make ext/int links | `SimpleNetwork.cc:106-162` |
| Routing policy | `WeightBased.cc:73-127`; `SimpleNetwork.py:126-128` |
| Topology wiring (mesh/ext links, latencies, bw) | `CustomMesh.py:60-184,200-282,305-314,384-391,399` |
| `--simple-physical-channels` | `configs/network/Network.py:277-282` |
| CHI network params | `CHI.py:236-242`; `CHI_config.py:113-115` |
| noc_config knobs | `rbook_4x4.py:21-33` |

---

### Verification

All structural and numeric claims were cross-checked against a freshly generated
`config.ini` for the exact command in §1: 16 `Switch` routers; mesh router
`latency=4, int_routing_latency=4, ext_routing_latency=6`; int links
`latency=2`, their buffers `buffer_size=3, max_dequeue_rate=1,
allow_zero_latency=false`; ext links `latency=1, bandwidth_factor=32`;
`port_buffers buffer_size=4, max_dequeue_rate=0, allow_zero_latency=true`;
network `buffer_size=4, endpoint_bandwidth=1000, number_of_virtual_networks=4,
control_msg_size=8, data_msg_size=32, physical_vnets_channels=1 1 1 1,
randomization=false`.
</content>
</invoke>
