# CreditedLinkBuffer — Design Specification

A credit-based, latency-accurate, **head-of-line-eliminating** link buffer for
gem5's Ruby **SimpleNetwork**. It models a single point-to-point
flow-controlled channel endpoint: the **receiver-side input buffer** of a
switch/XP together with the **producer-side credit accounting** for the link
that feeds it.

This document is the reference design: storage, latency, and credit models; the
producer/consumer scheduling and notification protocol; the public API; the
causality rules; the invariants; and the rationale, with citations to the
codebases the design draws from (Sparta, SystemC/TLM-2.0, BookSim 2, gem5
Garnet).

## Status: fixed semantics, open backend

This spec deliberately separates two layers:

- **Fixed (the contract).** The credit FSM, credit-return-on-departure timing,
  the producer/consumer interface, the causality rules, the RTT sizing rule, and
  the feature-off byte-identity. These are normative — change them and you are
  modelling something else.
- **Open (the implementation).** The storage container, whether maturation is a
  separate stage or a predicate, and whether the class *extends* `MessageBuffer`
  or is *standalone*. Several backends satisfy the same contract; §4 and §11
  enumerate them with trade-offs. Because the credit FSM is decoupled from
  storage (P11), this choice is deferrable and swappable.

---

## 1. Motivation and Scope

### 1.1 The goal

We are evolving gem5's Ruby **SimpleNetwork** into a model of a realistic **AMBA
CHI / CMN-style interconnect** — specifically the input buffering and crosspoint
(XP) behaviour of a packet-switched mesh. SimpleNetwork was chosen over Garnet
for its message (= packet) granularity and modelling productivity: for
single-flit CHI traffic it sits at the right abstraction level. But to make it
behave like a *real* CHI link and XP, it must reproduce the link-level mechanics
a real NoC has — and that SimpleNetwork today does not. This component supplies
exactly those mechanics as one reusable building block.

### 1.2 What a real CHI/CMN link + XP does (the reference behaviour)

- **Credit-based flow control.** The sender holds *credits* equal to the number
  of free slots in the downstream input buffer. A send consumes a credit; when a
  slot frees, the receiver sends a *credit* back; that credit travels over a
  **return latency** before the sender may reuse it.
- **Finite per-VC input buffering, sized to the bandwidth–delay product.** To
  keep a link full you need `credits ≥ round-trip time`; with fewer credits the
  link throttles even when the receiver is draining freely.
- **Head-of-line-eliminating arbitration.** At the XP, a packet bound to a
  *free* output is served even if an older packet bound to a *busy* output sits
  ahead of it. (The target RTL router does this.)
- **Three physically distinct realities:** forward wire delay, buffer occupancy,
  and the backward credit-return delay are independent quantities.

### 1.3 The problems — what SimpleNetwork cannot model today

The root cause is that a single `MessageBuffer` at a SimpleNetwork switch input
plays **three fused roles** (`src/mem/ruby/network/simple/{PerfectSwitch,Throttle}.cc`):
it is the *delay line* (the `delta` to `enqueue()`, `Throttle.cc:203`), the
*input storage* (the queue `PerfectSwitch` drains, `PerfectSwitch.cc:200,247`),
**and** the *flow-control signal* (its finite capacity, checked instantaneously
via `areNSlotsAvailable()`, `PerfectSwitch.cc:216` / `Throttle.cc:186`). On top
of that, `PerfectSwitch` is **head-only** — it never looks past the head of an
input buffer. The concrete consequences:

| # | Problem | Consequence — what you cannot model today |
|---|---------|-------------------------------------------|
| **G1** | **Zero-latency credit return.** A freed slot is visible to the producer the same cycle the consumer dequeues. | No credit-return latency; cannot reproduce throughput throttling when `credits < RTT`, nor the *timing* of congestion propagation. |
| **G2** | **Credits == capacity.** The only backpressure knob is the physical buffer depth. | Cannot set credit count independently of storage; cannot study credit/buffer sizing or the bandwidth–delay product. |
| **G3** | **Wire, storage, and flow-control are one knob.** | Cannot separate "slow because far" (latency) from "slow because congested" (backpressure) — they share a parameter. |
| **G4** | **Head-only switching (no HoL elimination).** | A head bound to a busy output stalls everything behind it, including packets to *free* outputs; HoL and congestion effects are under-modelled, and a HoL-eliminating RTL XP cannot be represented at all. |
| **G5** | **No reusable, instrumented primitive.** | Each experiment re-derives ad-hoc buffering; want one building block whose stats/probes/checkpointing come for free and that can be iterated on. |

### 1.4 What `CreditedLinkBuffer` provides

| Capability | Solves |
|------------|--------|
| An explicit **credit pool** with a **configurable credit-return latency**, decremented on send and incremented only on a *delayed* event. | G1 |
| Credit count **decoupled from buffer capacity** (`credits` independent of `buffer_size`, sized to `RTT`). | G2 |
| **De-conflated** wire delay (`t_link`), storage, and credit signal as separate parameters. | G3 |
| **Out-of-order (OOO) selection** so a packet to a free output is served past a head to a busy one. | G4 |
| Built on / beside `MessageBuffer` machinery (or a focused standalone class) so **instrumentation is reused**. | G5 |

### 1.5 Scope

**In scope:** one producer, one consumer, one credit pool (a single VC / flow),
with OOO selection for HoL elimination within that flow.
**Out of scope (compose, don't bake in):** multiple VCs and switch arbitration —
those live in the *router/XP*, built from one `CreditedLinkBuffer` per VC/flow.

---

## 2. Design Principles (and where they come from)

| # | Principle | Source / evidence |
|---|-----------|-------------------|
| P1 | **Separate the three concerns** — wire delay, storage, credit return are independent parameters. Fusing them makes "slow because far" indistinguishable from "slow because congested," and forces buffer-depth = link-latency. | SystemC keeps `sc_fifo` (storage, zero-delay) separate from PEQ delay channels and from credit events. Garnet uses a separate `CreditLink` with its own `m_latency`. |
| P2 | **Credit counter is producer-side state; return *deltas*.** The upstream owns the count of free downstream slots; the downstream only emits "+N freed". Neither side reads the other's occupancy across the link (that would be acausal). | Sparta: producer holds `credits_`, `+=` on return, `--` on send. Garnet `OutVcState` lives on the upstream `OutputUnit`. BookSim `BufferState` lives on the sender. |
| P2′ | **Hosting vs. semantics.** In gem5 the producer holds a pointer to the downstream buffer, so the credit counter may be *hosted on the buffer object* for convenience. This stays causal **iff** the counter is only incremented by *delayed* events — the lag is in the increment timing, not the read. | This conversation. |
| P3 | **Return the credit when the message *departs to the output*, not when it arrives, and not when it merely matures.** A credit represents an input-buffer slot held until the message wins arbitration and crosses to the output. | Garnet generates the credit in the downstream `SwitchAllocator::arbitrate_outports` — exactly at departure (`SwitchAllocator.cc:246`). Matches RTL: credit returns when the item won arbitration and moved to the output buffer. |
| P4 | **Credit return is a real, separately configurable latency.** | Garnet `CreditLink::m_latency`; BookSim separates wire `_delay` from processing `_credit_delay`. |
| P5 | **Size credits to the round-trip time, not to memory.** Throughput caps at `min(1, credits/RTT)`. Storage must be ≥ credits so the *credit counter*, never the container, is the binding constraint. | Dally & Towles ch. 13 (`F_min = t_rt·b/L_f`); BookSim `vc_buf_size` independent of `buf_size`; Garnet `m_max_credit_count` independent of `flitBuffer`. |
| P6 | **Break the zero-time feedback loop:** forward (data) latency must be ≥ 1 cycle so a credit→send→credit chain cannot recurse within one tick. | Sparta: data ports carry ≥1-cycle delay, credit ports 0. |
| P7 | **Make same-tick credit visibility an explicit, consistent rule.** An off-by-one shifts effective RTT by a full cycle. | SystemC `notify(SC_ZERO_TIME)` → next delta; BookSim `+_delay-1`; Garnet routes all through `clockEdge()`. |
| P8 | **Guard the counter with asserts** (`0 ≤ credits ≤ max`) — a leak inflates bandwidth, a lost credit deadlocks. | Garnet `OutVcState` asserts `>=0` on decrement, `<=max` on increment. |
| P9 | **Opt-in, default-off / feature-isolated.** With credits disabled (or for plain `MessageBuffer`s), behaviour is byte-identical to today, so every protocol controller and regression is untouched. | This conversation; mirrors the `SwitchPortBuffer(MessageBuffer)` pattern (`SimpleNetwork.py:96`). |
| P10 | **Self-collapsing per-cycle wake**, not a reschedule per event. | Sparta `UniqueEvent`/`SingleCycleUniqueEvent`. |
| P11 | **Fix the semantics, keep the backend open.** The credit FSM and the producer/consumer contract are independent of the storage container and of whether the class extends `MessageBuffer` or is standalone. Choose the backend per need; it is swappable. | This conversation (storage/inheritance debate). |

---

## 3. Structure

```
        PRODUCER  (upstream XP / Throttle)
            |
            |  (1) hasCredit(n)?  --no-->  stall; woken later by credit callback (6)
            |  (2) yes: enqueue(msg, now, t_link)  ->  credits -= n     (spend at send)
            v        ...the message becomes "ready" at now + t_link  (wire delay, >=1)

   CreditedLinkBuffer  (one per link/VC; storage backend is an OPEN choice, see Sec.4)
   +--------------------------------------------------------------------------+
   |   CREDIT POOL (producer-view):   credits = 3 / max = 5   [#][#][#][.][.]  |
   |                                                                          |
   |   STORAGE  (holds each item from enqueue until it departs to an output): |
   |      * maturing : items not yet ready (ready at enqueue_tick + t_link)   |
   |      * ready    : eligible items = the arbitration set                   |
   |      occupancy = maturing + ready ;  capacity >= credits   (P5)          |
   +--------------------------------------------------------------------------+
            |
            |  (3) arbitration @ now:  selectEligible(out j) ->
            |        OLDEST ready item whose route == j  (skips items bound to
            |        busy/other outputs  ==>  head-of-line ELIMINATION)
            v
        CONSUMER  (downstream XP arbiter / crossbar)
            |
            |  (4) popAt(winner)  ->  item DEPARTS to the output  (OOO removal)
            |        -> schedule CreditReturnEvent @ now + t_creditReturn   (P3, P4)
            v
        CreditReturnEvent fires (delayed by t_creditReturn):
            (5) credits += n          (assert credits <= max)
            (6) fire creditCB  ->  wake the stalled producer

   Latencies:   t_link         = forward wire delay   (>= 1 cycle, P6)
                t_creditReturn = credit return delay   (independent of t_link, P4)

   RTT_min    =  t_link + 1 (min dwell) + t_creditReturn
   throughput =  min( 1 ,  credits / RTT_min )   slots/cycle
   =>  size credits >= RTT_min for full bandwidth (P5); fewer credits throttle the link.
```

The component owns **two** logical pieces of state: the **storage** (items from
enqueue to departure, internally split into *maturing* and *ready*) and the
**credit pool**. The **wire delay** is not stored — it is the `t_link` the
producer passes to `enqueue()`. The **credit return delay** `t_creditReturn` is
a parameter. How the storage is realized is deliberately open (§4).

---

## 4. Storage Model — contract and backend options

### 4.1 The container-agnostic contract (FIXED, P11)

The credit FSM (§6) and the consumer/producer API (§7–§8) depend only on this
contract. *Any* backend that satisfies it is valid:

| Operation | Meaning |
|-----------|---------|
| `insert(msg, ready_tick)` | admit an item that becomes eligible at `ready_tick = enqueue_tick + t_link` (supports per-item / variable delay). |
| `isReady(now)` | does any item with `ready_tick ≤ now` exist? |
| `selectEligible(pred, now)` | return a handle to the **oldest** item with `ready_tick ≤ now` satisfying `pred` (e.g. `route(msg)==output && downstreamHasCredit`). Oldest-first preserves per-(input,output) order while letting the arbiter skip items bound elsewhere → **HoL elimination**. |
| `remove(handle)` | remove the selected item **from anywhere** in the buffer (the winner need not be the head). |
| `occupancy()` | total items held = maturing + ready; **`capacity ≥ credits`** (P5). |

Plus: **single producer / single consumer per credit pool** (P2; SystemC
`sc_fifo` contract). Multi-source links use one `CreditedLinkBuffer` per source.

Note what the contract does **not** fix: whether "maturing" is a distinct
physical stage, what container holds the ready set, and how `remove` is
implemented. Those are open (§4.2).

### 4.2 Backend options (OPEN — pick per need)

**Option A — two-stage: maturation heap + ready deque (reuse-maximizing).**
Keep `MessageBuffer`'s inherited `m_prio_heap` as the *maturation* stage (it is
good at time-ordered release and variable per-item delay), and, on access, move
matured items (`ready_tick ≤ now`) into a separate **ready deque** that supports
OOO scan + erase. Reuses the heap, the existing maturation/wakeup scheduling,
and all instrumentation. `selectEligible`/`remove` operate on the deque;
`insert` feeds the heap. (This is the "extend `MessageBuffer`" path, §11.A.)

**Option B — single ready container with `ready_tick` (simplicity-maximizing).**
Hold all items in one container (deque / small vector / intrusive list); each
entry carries its `ready_tick`; *maturation is just a predicate* (`ready_tick ≤
now`) inside `selectEligible`. No separate heap, no move step. At NoC buffer
depths (≤ tens) the linear scan is trivial, so the two-stage split buys little.
Works either as an extension or standalone.

**Orthogonal axis — extend vs standalone:**

| | Extend `MessageBuffer` | Standalone SimObject |
|--|------------------------|----------------------|
| Reuse | heap, maturation scheduling, consumer wakeup, stats, functional access, serialization | none — re-implement the surface you need |
| Cleanliness | inherits unused protocol machinery (stall map, recycle, deferred msgs) | single responsibility |
| Obligation | override `functionalRead/Write` + `serialize` so the *added* ready container is not invisible; occupancy/credit count must span both containers | implement `functionalRead/Write`, `serialize`, stats from scratch |
| Substitutability | already a `MessageBuffer` (drop-in where the network expects one) | needs the network to hold the concrete type or a shared base |

**Decision guidance (not a mandate):**
- Prioritizing reuse / minimal disruption, and you like the heap doing
  maturation → **A (extend, two-stage)**.
- Prioritizing a clean single-purpose container, and a scan-per-cycle is fine at
  your depths → **B**, standalone or extended.
- Because of the §4.1 contract and P11, you can start with one and swap later;
  the credit FSM and the XP-facing API do not change.

---

## 5. Latency Model — three independent latencies

| Latency | Symbol | Where it lives | Models |
|---------|--------|----------------|--------|
| **Forward / wire** | `t_link` | `enqueue(msg, now, t_link, …)` `delta` (producer-supplied, as Throttle does today) | Time to traverse the link and land in the buffer. **Must be ≥ 1 cycle (P6).** |
| **Dwell** | `dwell` | emergent: time from `ready` until the item wins arbitration and is `popAt`'d | Router pipeline / arbitration / contention. ≥ 1 cycle. |
| **Credit return** | `t_creditReturn` | `credit_return_latency` parameter | Time for the freed-slot credit to reach the producer **after departure**. Configurable, independent of `t_link`. |

The **round-trip time** a single credit is "out of circulation" is

```
RTT = t_link + dwell + t_creditReturn          (+ any 1-cycle apply overhead)
```

**For credit *sizing*, use the no-contention minimum** `RTT_min = t_link + 1 +
t_creditReturn`. Under contention `dwell` grows, but then the consumer — not the
credit loop — is the bottleneck, so the larger RTT does not demand more credits.

Sustained per-flow throughput is `min(1, credits / RTT_min)` slots/cycle (one
slot = one single-flit message for the CHI NoC). This is the **bandwidth–delay
product** rule (P5): you need ≥ `RTT_min` credits to keep an *uncongested* link
full; fewer credits idle it `(RTT_min − credits)/RTT_min` of the time even when
the consumer drains freely. Reproducing that throttle is the reason the
component exists.

---

## 6. Credit Model

### 6.1 State

```
credits      : current credits the producer may still spend   (0 … max_credits)
max_credits  : credit pool size = downstream admission limit   (the P5 knob)
```

`credits` is initialized to `max_credits` at `startup()` (link begins fully open
— Sparta "send initial credits", P2).

### 6.2 Decrement on send, increment on delayed departure (P3)

- **Decrement (at send).** The producer calls `enqueue(msg, now, t_link, …)`
  only after `hasCredit(n)` returns true. `enqueue` asserts the credit, does
  `credits -= n`, and `insert`s the item (eligible at `now + t_link`). The
  credit is spent at *send* time, reserving a future slot so two in-flight items
  cannot oversubscribe (hence `buffer_size ≥ credits`).
- **No credit on maturation.** When a maturing item becomes ready (Option A's
  heap→deque move, or simply `ready_tick ≤ now` in Option B), **no credit is
  returned** — the item still occupies an input-buffer slot.
- **Increment (at departure + return latency).** When the consumer wins
  arbitration and removes the item with `popAt` at `t_depart`, the buffer
  schedules a **`CreditReturnEvent`** at `t_depart + t_creditReturn`. When it
  fires: `credits += n` (assert `≤ max_credits`, P8) and **notify the
  producer** (§7).

This timing is the load-bearing point: the credit is held for the item's full
residency (maturing **and** ready) and returns **only when it wins arbitration
and crosses to the output** — exactly matching an RTL credited input buffer.
Returning earlier (on arrival, on maturation, or on a demux into per-output
lanes) shortens the apparent RTT and over-admits.

### 6.3 Granularity

Credits count **slots** (one per single-flit CHI packet). `n` defaults to 1.
For variable-size messages, `n = ceil(bytes / slot_bytes)`. (For the CHI NoC,
everything is single-flit, so `n == 1`.)

---

## 7. Producer Scheduling and Notification

```cpp
if (link.hasCredit(n)) {                       // gate (replaces areNSlotsAvailable, P5)
    link.enqueue(msg, now, t_link, rnd, warmup);   // spend credit + insert with wire delay
}                                              // else: stall — do NOT drop (backpressure)
link.registerCreditCallback(producerWake);     // fired on delayed credit return
```

On a `CreditReturnEvent` the buffer increments `credits` and invokes
`producerWake`, which reschedules the producer's send attempt. To avoid a
reschedule storm, `producerWake` pokes a **self-collapsing per-cycle event**
(P10) that re-arms next cycle only while `hasCredit() && producer has traffic`.

**Backpressure propagation (no drops).** If the consumer cannot drain (its own
downstream is credit-starved), it stops winning arbitration → no
`CreditReturnEvent`s fire → `credits` stays 0 → the producer stalls → *its*
upstream credits drain. Congestion propagates hop-by-hop through credit
starvation, with realistic timing instead of the current instantaneous check.

---

## 8. Consumer Scheduling and Notification

The consumer (downstream XP) is woken by the existing maturation/consumer
mechanism when an item becomes ready. It then arbitrates with OOO selection:

```cpp
link.maybeMature(now);                          // Option A: heap->deque; Option B: no-op
// For each output port j the arbiter is resolving this cycle:
auto h = link.selectEligible(
            [&](const Message& m){ return route(m) == j && out[j].hasCredit(); }, now);
if (h.valid()) {
    crossbarSend(out[j], link.peekAt(h));
    link.popAt(h, now);          // OOO removal; schedules CreditReturnEvent @ now + t_creditReturn
}
```

`selectEligible` returns the **oldest** ready item routing to `j`, so a message
to a free output is served even when an older message to a *busy* output sits
ahead of it — **head-of-line elimination** — while per-(input,output) order is
preserved. The consumer never touches credits directly; `popAt` (departure) is
what *causes* the delayed credit return (P2/P3).

A **head-only** consumer (no HoL elimination) is the degenerate case: always
select the head if its output has credit. That models a plain VC-FIFO XP and
needs no OOO. Choose per the XP you are modelling (§10).

---

## 9. Causality and Same-Tick Rules (P7)

1. **Forward latency ≥ 1 (P6).** `enqueue` with `t_link == 0` is rejected/clamped
   in credited mode. An item cannot be sent, matured, won, and credit-returned
   within one tick, so a 1-credit link cannot spuriously sustain full rate.
2. **A spent credit always returns strictly later than its own send.** Since
   `t_link ≥ 1`, the earliest departure is `send + 1`, and the credit returns at
   `depart + t_creditReturn` — never visible to the send that spent it. No
   acausal self-credit regardless of `t_creditReturn`.
3. **Credit returns land on a clock edge with defined priority.**
   `CreditReturnEvent` is scheduled at `clockEdge(t_depart + t_creditReturn)`, at
   an `EventQueue` priority **earlier** than producer send arbitration in that
   tick, so a return due at tick *T* is seen consistently by every send decision
   at *T*. Document the chosen priority next to the event class.
4. **`credit_return_latency` semantics.** Additional delay beyond departure.
   `== 1` ⇒ credit usable the cycle after departure; `== 0` ⇒ same edge the item
   departed (a 1-cycle minimum back-channel still holds because departure is a
   clock-edge event). True zero-cost credit return (today's behavior) is the
   **disabled** mode (`credits == 0`), not `credit_return_latency == 0`.

---

## 10. Head-of-Line Elimination — model options

The user's target XP eliminates HoL, so OOO selection is the primary model;
alternatives are documented for completeness. All are storage-backend choices
under the §4.1 contract.

**(1) OOO selection from a single ready set — recommended for HoL-eliminating
XPs.** One buffer per VC; the arbiter `selectEligible`s the oldest ready item to
a free output and `popAt`s it. The item stays in the input buffer until it wins,
so the **credit returns exactly at departure** (P3) — correct RTL timing. OOO
`remove` is the only special storage requirement, satisfied by either §4.2
backend (deque erase, or heap-vector erase). At NoC depths the scan/erase is
cheap. This is the design the rest of this spec is written around.

**(2) Per-output FIFO lanes (demux on enqueue) — not recommended here.** Route
each item into `lane[output]` on arrival; each lane is head-only FIFO; the
arbiter peeks lane heads. Avoids arbitrary removal, **but**: (a) needs
`N_out × N_vc` queues per input; and (b) the credit boundary is wrong unless the
*lanes themselves* are the credited storage — popping from a credited buffer to
demux into lanes returns the credit *before* arbitration (P3 violation). Making
the lanes the credited storage re-introduces (a)'s complexity. Prefer (1).

**(3) Head-only FIFO (no HoL elimination) — simplest.** Serve only the head if
its output has credit. No OOO, no lanes; models a VC-FIFO XP where VCs (e.g. the
CHI TgtID split) provide flow separation and HoL within a VC is real. Valid when
the modelled XP does *not* eliminate HoL.

Per-VC structure is always by **composition**: one `CreditedLinkBuffer` per VC,
each with its own credit pool, so a starved VC never blocks siblings (Garnet /
BookSim per-VC credits). HoL elimination, when used, is *within* a VC's buffer
via option (1).

---

## 11. Proposed API

The **interface and behaviour are fixed**; the two skeletons below are the two
implementation options from §4.2. Pick one; the XP-facing API is identical.

### 11.1 Fixed interface

```cpp
// ---- Producer side ----
bool     hasCredit(unsigned n = 1) const;          // !enabled || credits >= n
unsigned curCredits() const;
unsigned maxCredits() const;
void     registerCreditCallback(std::function<void()> cb);

// ---- Consumer side ----
void   maybeMature(Tick now);                      // Option A: heap->deque; Option B: no-op
bool   isReady(Tick now) const;                    // any item ready_tick <= now
using  Handle = /* stable handle into the ready set */;
Handle selectEligible(const std::function<bool(const Message&)>& pred, Tick now) const;
const Message* peekAt(Handle h) const;
void   popAt(Handle h, Tick now, unsigned n = 1);  // OOO remove; schedule credit return

// ---- Insert (producer) ----
//   real MessageBuffer signature (MessageBuffer.hh:127):
//   enqueue(MsgPtr, Tick curTime, Tick delta, bool ruby_is_random,
//           bool ruby_warmup, bool bypassStrictFIFO=false);
//   credited path: assert hasCredit(1) && delta>=1cy; credits-=1; insert(msg, curTime+delta).

// ---- Params (MessageBuffer.py or a standalone .py) ----
//   credits               (0 => disabled; <= buffer_size)
//   credit_return_latency (cycles after departure; default 1)
//   enable_ooo_pop        (allow selectEligible/popAt; else head-only)
```

### 11.2 Option A — extend `MessageBuffer` (two-stage)

Add a ready deque beside the inherited heap; `enqueue`/`dequeue` gain guarded
branches (they are non-virtual, so this is a guarded branch *inside* them, not an
override). `CreditedLinkBuffer` is a Python subclass setting the params.

```cpp
// new members
bool     m_credit_enabled;          // (credits != 0)
unsigned m_credits, m_max_credits;
Cycles   m_credit_return_latency;
std::deque<Entry> m_ready;          // matured items (Entry = {MsgPtr, ready_tick})
std::function<void()> m_credit_cb;

// enqueue(): if (m_credit_enabled){ assert(hasCredit(1)); assert(delta>=1cy); --m_credits; }
//            <existing insert into m_prio_heap>
// maybeMature(now): pop heap entries with time<=now into m_ready
// popAt(h,now,n): erase from m_ready; scheduleCreditReturn(n, now)
// onCreditReturn(n): m_credits+=n; assert(<=max); if (m_credit_cb) m_credit_cb();
// OVERRIDE functionalRead/Write + serialize to cover BOTH m_prio_heap AND m_ready
// occupancy / credit accounting span both containers
```

### 11.3 Option B — standalone (single ready container)

```cpp
class CreditedLinkBuffer : public SimObject /* or a shared RubyBuffer base */ {
    std::deque<Entry> m_items;       // Entry = {MsgPtr, ready_tick}; maturation = predicate
    unsigned m_credits, m_max_credits; Cycles m_credit_return_latency;
    std::function<void()> m_credit_cb;
    // insert/isReady/selectEligible(pred,now: ready_tick<=now && pred)/popAt as above
    // implement functionalRead/Write + serialize + stats from scratch
};
```

### 11.4 Behavioural contract (both options)

| Call | Effect |
|------|--------|
| `hasCredit(n)` | `true` if disabled, else `credits >= n` |
| `enqueue(…, delta, …)` (enabled) | `assert(hasCredit(1) && delta>=1cy)`; `credits-=1`; `insert(ready=now+delta)` |
| `selectEligible(pred, now)` | oldest ready item satisfying `pred`, or invalid handle |
| `popAt(h, now, n)` | OOO remove; `scheduleCreditReturn(n, now)` |
| `onCreditReturn(n)` | `credits += n`; `assert(credits <= max)`; `credit_cb()` |
| disabled (`credits==0`) | byte-identical to plain `MessageBuffer` |

---

## 12. Lifecycle Walkthrough (one message + its credit)

```
 t0      Producer: hasCredit(1)==true -> enqueue(m, t0, t_link=2)
         credits: 3 -> 2            (spent at send, §6.2)
 t0+2    m matures (ready); enters the ready set; consumer woken
 t0+2..  m waits while its output is busy / loses arbitration   <- dwell
         (HoL elimination: other ready items to free outputs depart meanwhile)
 t5      m wins arbitration -> popAt(m) -> departs to output;
         schedule CreditReturn @ t5 + 3
 t5      credits unchanged (still 2)        <- NOT restored until departure+return (P3)
 t8      CreditReturnEvent: credits 2 -> 3; producer wake fires
         (visible to producer sends at the t8 edge, P7-3)

 Credit "out" t0..t8  =>  loop RTT = 8 = t_link(2) + dwell(3) + t_creditReturn(3).
 If max_credits < uncongested RTT_min (= t_link + 1 + t_creditReturn = 6),
 the link cannot stay full even when the consumer drains freely.
```

---

## 13. Invariants and Asserts (pitfalls, P8)

- `0 ≤ credits ≤ max_credits` — assert on every mutation. Underflow ⇒ oversend
  (gate bug); overflow ⇒ **double credit return / leak**.
- `max_credits ≤ buffer_size` — checked at construction (P5).
- `t_link ≥ 1` in credited mode (P6) — checked in `enqueue`.
- **Exactly one `CreditReturnEvent` per `popAt`.** Missing ⇒ credit leak →
  eventual deadlock; duplicate ⇒ inflated bandwidth + overflow assert.
- **Credit returns on departure, never on maturation.** A return scheduled at
  the heap→deque move (Option A) or at `ready_tick` (Option B) is a P3 bug.
- `credits == 0` ⇒ feature disabled: no `CreditReturnEvent` ever scheduled;
  behaviour equals plain `MessageBuffer` (regression safety, P9).
- Option A only: occupancy, credit accounting, `functionalRead/Write`, and
  `serialize` must span **both** the heap and the ready deque.

---

## 14. Instrumentation

- **Option A (extend):** inherits all `MessageBuffer` stats, debug flags
  (`RubyNetwork`), functional access, serialization — *provided* the overrides
  cover the ready deque (§13).
- **Option B (standalone):** implement the functional/serialize surface and the
  stats you want; optionally a shared helper.

Add either way, into the buffer's `statistics::Group` at construction (Sparta
auto-stats lesson):
- `m_credit_stalls` — events with traffic but `credits == 0`.
- `m_credit_occupancy` — running average of `max_credits − credits`; histogram
  bucketed `0 … max_credits`.
- `m_credit_returns` — total `CreditReturnEvent`s; must equal total `popAt`s at
  drain (leak detector).
- Storage occupancy (maturing/ready); the gap vs credit-occupancy shows the
  bandwidth–delay-product slack.

---

## 15. Integration with SimpleNetwork

- **Where:** the inter-switch **link buffers** created in
  `SimpleLink.setup_buffers` (`SimpleLink.py:67`) become `CreditedLinkBuffer`s
  (one per VC). Intermediate `SwitchPortBuffer` staging buffers stay vanilla
  (`credits == 0`).
- **Gate swap:** at `PerfectSwitch.cc:216` / `Throttle.cc:186` replace
  `out->areNSlotsAvailable(1)` with `out->hasCredit(1)` (returns `true` for
  non-credited staging, so one call site serves both).
- **Arbitration:** the XP's per-output selection calls `selectEligible`/`popAt`
  (HoL elimination) instead of the current head-only `peekMsgPtr`/`dequeue`
  loop. If you build a bespoke XP, it can hold `CreditedLinkBuffer` directly —
  then no `MessageBuffer` substitutability question arises (the network↔
  controller edge keeps using `MessageBuffer`).
- **Wire delay:** unchanged — Throttle passes `m_link_latency` as the `enqueue`
  delta; that *is* `t_link`.
- **Throttle role:** still models per-link **bandwidth**; credits model
  **buffering/backpressure** — orthogonal knobs, as in real hardware.
- **Producer wake:** route the credit callback into the XP's existing
  wake/reschedule path so the producer wakes on credit return, not by polling.

---

## 16. Validation Plan

1. **Disabled-mode identity:** `credits == 0` → full Ruby regression suite +
   CHI testbench (`run-memset`, `run-ping_pong`) diff byte-identical (P9).
2. **Throughput knee:** single producer→consumer, always-ready consumer. Sweep
   `credits` at fixed `RTT_min`; assert sustained throughput
   `= min(1, credits/RTT_min)`, knee at `credits == RTT_min` (P5). Proves the
   latency is real, not instantaneous.
3. **Credit conservation:** at drain, `m_credit_returns == total popAt` and
   `credits == max_credits` (no leak).
4. **HoL elimination:** mix two flows in one buffer, one bound to a blocked
   output; confirm the other flow still departs (would stall under head-only).
5. **Backpressure propagation:** chain three credited buffers; stall the tail;
   confirm credit starvation walks upstream with the expected per-hop delay.
6. **Backend equivalence:** Options A and B with identical params must produce
   identical latency/throughput traces (validates the §4.1 contract / P11).
7. **A/B vs Garnet:** matched single-flit, per-VC-credit config; latency–
   throughput curves agree within modelling tolerance.

---

## 17. Rationale Summary (per source)

- **Sparta:** producer-held credit counter; return deltas; data ≥1 / credit 0
  latency; self-collapsing per-cycle wake; auto-stats from the storage
  constructor; `Buffer<T>` for OOO erase with stable iterators.
- **SystemC/TLM:** never fuse wire/storage/credit; model each latency as an event
  at an absolute future time; retry on no-credit, never drop; notify deferred a
  delta, never same-evaluate.
- **BookSim/Garnet:** return the credit on **departure** over a separate
  latency-bearing channel; credit count decoupled from container size; size to
  RTT; per-VC pools so a starved VC never blocks siblings; assert leak/overflow.

---

## 18. References

- gem5 Garnet (local): `src/mem/ruby/network/garnet/{Credit.hh, OutVcState.cc,
  OutputUnit.cc, InputUnit.cc, SwitchAllocator.cc, NetworkLink.cc,
  CreditLink.hh}`.
- gem5 SimpleNetwork (target): `src/mem/ruby/network/simple/{Throttle.cc,
  PerfectSwitch.cc, SimpleLink.py, SimpleNetwork.py}`; `MessageBuffer.{hh,py}`.
- Sparta: `sparta::DataOutPort/DataInPort`, `Buffer`, `Queue`
  (https://sparcians.github.io/map/communication.html,
  https://sparcians.github.io/map/classsparta_1_1Buffer.html); core_example
  `Decode.{hpp,cpp}`; olympia `Dispatch.{hpp,cpp}`
  (https://github.com/riscv-software-src/riscv-perf-model).
- SystemC/TLM: Accellera `sc_fifo.h`; `tlm_utils::peq_with_get` /
  `peq_with_cb_and_phase`
  (https://github.com/accellera-official/systemc; Doulos TLM-2.0 notes).
- BookSim 2: `src/{buffer_state,channel,credit,creditchannel,flitchannel}.*`,
  `src/routers/iq_router.cpp` (https://github.com/booksim/booksim2).
- W. Dally & B. Towles, *Principles and Practices of Interconnection Networks*,
  ch. 13 (Flow Control) — credit sizing `F_min = t_rt·b/L_f`, i.e.
  `credits ≥ RTT`.
```
