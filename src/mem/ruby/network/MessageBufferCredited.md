# Credited MessageBuffer API + consumer-side OoO adapter

This document describes the **local extensions** to `MessageBuffer` (see the
`<local-addition ...>` markers in [`MessageBuffer.hh`](MessageBuffer.hh)) that
turn an ordinary Ruby message buffer into a **credited link buffer** with a
**decoupled credit-return API**, and explains how the XP simple-network switch
uses that buffer together with a **consumer-side out-of-order (OoO) adapter** to
get head-of-line-avoiding arbitration without putting any selection machinery in
the buffer itself.

The design follows a strict **separation of concerns**:

| Concern | Owner | Mechanism |
| --- | --- | --- |
| **Flow control** — may a producer admit a message, and when does the slot free up? | `MessageBuffer` | credit pool, `spendCredit` on `enqueue`, `returnCredit()`, `hasCredit`/`areNSlotsAvailable` |
| **Ready-message organization** — which matured message does the consumer take next, and how are they held while waiting? | the consumer (XP switch) | per-input random-access "ready" container, OoO arbitration |

Everything here is opt-in. When `credits == 0` (the default), a `MessageBuffer`
behaves exactly like upstream: infinite/finite slot accounting, strict-FIFO
head-only `dequeue()`, and `returnCredit()` is a no-op. The buffer is **always
strict-FIFO** — it has no notion of out-of-order selection; that lives entirely
in the consumer.

The canonical consumer is the XP simple-network switch
([`simple/xp/XPSwitch.cc`](simple/xp/XPSwitch.cc)); the credited-buffer contract
is pinned by
[`simple/xp/credited_link_buffer.test.cc`](simple/xp/credited_link_buffer.test.cc).

---

## 1. Parameters ([`MessageBuffer.py`](MessageBuffer.py))

```python
credits               = Param.Unsigned(0, "Credit pool size; 0 disables credits")
credit_return_latency = Param.Cycles(1,
    "Cycles from downstream departure to upstream credit visibility")
```

Construction-time invariants (enforced in `CreditState`'s constructor, fatal on
violation):

- `buffer_size == 0 || buffer_size >= credits` — a credited buffer may be
  unbounded, but if it is bounded it must hold at least `credits` slots so an
  admitted message always has somewhere to land.
- `credit_return_latency != 0` for credited buffers — credit return must take at
  least a cycle (a 0-cycle return would let a credit be reused in the same cycle
  it is freed, which is not a real link).

`m_credit` is a `std::unique_ptr<CreditState>`; it is `nullptr` exactly when
`credits == 0`. `isCredited()` is just `m_credit != nullptr`.

> **There is no `enable_ooo_pop` parameter on the buffer.** Out-of-order pop was
> removed from `MessageBuffer`; the XP switch carries an `enable_ooo_pop`
> parameter instead ([`xp/XPNetwork.py`](simple/xp/XPNetwork.py),
> `XPSwitch.enable_ooo_pop`), because OoO is a consumer policy, not a buffer
> feature.

---

## 2. Credit model

A credited buffer starts with `credits` (== `maxCredits()`) available credits.

```
            enqueue() / spendCredit()         dequeue() (in-order, head-only)
producer  --------------------------->  buffer  ----------------------------> consumer
   ^             (-1 credit, immediate)          |  message moves into the
   |                                             |  consumer's ready container
   |                                             |
   |                    returnCredit(delay)  <---+  consumer calls this when the
   |                    (+1 after delay)            message actually departs
   +------------------------------------------------  downstream
```

- **Spend (immediate):** every `enqueue()` on a credited buffer calls
  `CreditState::spendCredit(delta)`, decrementing the pool by one. It is a fatal
  error (`panic`) to enqueue without an available credit, and `delta` must be
  non-zero. **The producer is responsible for checking credit availability
  before enqueueing** — the buffer does not block, it asserts.
- **Pop is credit-neutral:** `dequeue()` removes the head message and accounts
  occupancy/stats, but **does not touch credits**. This is the key decoupling:
  popping a message into the consumer's staging is *not* the same event as the
  message departing downstream, so it does not free the upstream slot.
- **Return (explicit, delayed):** the consumer calls
  `returnCredit(cur_time, credit_return_delay, slots)` when the message has
  genuinely left — for the XP switch, when it grants the message onward. Each
  return records the slots against their **maturity tick**
  (`cur_time + credit_return_delay`). Because a credited link uses a fixed return
  latency driven by a monotonic clock, maturity ticks are non-decreasing, so
  returns are kept in a `std::deque<{maturityTick, slots}>` — appended at the
  back (coalescing slots that share a tick) and redeemed from the front. Until a
  return matures its slots are counted as *pending* (`pendingCreditReturns()`),
  not available.
- **Notice (polled, lazy redemption):** there is **no** producer-side
  notification and **no scheduled event** when credits return. Matured returns
  are *pulled* back into the available pool by `redeemMatured()` at the top of
  every producer query — `hasCredit()`, `availableCredits()`,
  `areNSlotsAvailable()`, `spendCredit()`. A blocked producer learns about the
  freed credit by polling again on a later cycle (see
  [§4 Backpressure](#4-backpressure-handling-polling)); whichever cycle it next
  polls, any return whose maturity tick has passed is applied first. This is
  correct because `credit_return_latency` is non-zero, so a credit never matures
  in the tick it is queued, and under-counting between maturity and the next
  poll can only delay a grant, never over-grant. A push/event-driven alternative
  (needed only if a producer ever sleeps on a credit return) is sketched in
  [§5 Future work](#5-future-work-credit-return-callback).

Because the spend happens at admission and the return only on real departure,
the credit pool bounds **admitted-but-not-yet-departed** messages — i.e.
everything the consumer is still holding on the producer's behalf
(`input_buffer.occupancy + adapter.occupancy`), which is exactly what a credited
link is supposed to bound.

### Producer-side query API

```cpp
bool     isCredited() const;            // credits enabled for this buffer?
bool     hasCredit(unsigned slots = 1); // are >= slots credits available now?
unsigned availableCredits() const;      // credits free right now
unsigned maxCredits() const;            // pool size (== credits param)
Cycles   creditReturnLatency() const;   // configured return latency
unsigned pendingCreditReturns() const;  // credits freed but not yet visible
unsigned getMaxSize() const;            // buffer capacity (0 == unbounded)
```

On a **non-credited** buffer these degrade gracefully: `hasCredit()` returns
`true`, `availableCredits()/maxCredits()/pendingCreditReturns()` return `0`, and
`creditReturnLatency()` returns `Cycles(0)`. `getMaxSize()` reports the raw
`buffer_size` regardless of credits.

`areNSlotsAvailable(n, t)` is credit-aware: on a credited buffer it returns
`false` (and records a credit stall) when fewer than `n` credits are available,
*before* the normal slot/size check. So a producer that already gates on
`areNSlotsAvailable()` automatically respects credits.

### Credit-return API

```cpp
// Return `slots` credit(s) to the producer, becoming visible
// `credit_return_delay` ticks from `cur_time`. No-op on a non-credited buffer.
void returnCredit(Tick cur_time, Tick credit_return_delay, unsigned slots = 1);
```

This is the **only** path that returns credits. `dequeue()` never does.

---

## 3. Out-of-order is a consumer concern: the XP switch adapter

The buffer is strict-FIFO and head-only. To avoid head-of-line blocking — where
the oldest message is stuck behind a congested output while a younger message
could make progress — the **consumer** keeps its own random-access container of
matured messages and arbitrates over *that*. This is the "consumer-side OoO
adapter": the buffer owns admission and slot accounting; the switch owns how
ready work is held and chosen.

### Why this split (and not OoO inside the buffer)

A real router does not want a single ordered pool of ready messages; it wants to
*organize* them for the decision it is about to make. The XP switch already
demultiplexes by `(input port, vnet)`; a future router could split a ready set
**per output direction** (so each output arbiter sees only its own candidates),
**per QoS class** (per-class queues, deficit counters, aging state), or into any
other shape. The buffer cannot know which one its consumer wants, and it should
not grow an API for each. So the buffer hands the message over once it matures,
and the consumer's container shape is unconstrained.

This boundary already existed implicitly in the simple network — input buffers
(admission) vs. port/staging buffers (QoS, draining). The XP switch makes it
explicit: **input buffer = credits; ready container = selection.**

### The adapter in `XPSwitch` ([`simple/xp/XPSwitch.cc`](simple/xp/XPSwitch.cc))

Per `(input port, vnet)` the switch holds a `ReadyQueue`:

```cpp
struct ReadyQueue
{
    MessageBuffer       *source = nullptr;  // the credited, in-order input FIFO
    std::deque<MsgPtr>   ready;             // matured messages, arrival order
    size_t               capacity = 0;      // reorder-window bound (0 == inf)
    unsigned             grantsThisCycle = 0;
};
```

Each switch wakeup runs three stages (`XPSwitch::wakeup`):

1. **Drive** (`driveLinks` → `driveOutput`): move matured staging messages onto
   the downstream links (this enqueues into the next hop's credited input
   buffer, spending *its* credit).
2. **Drain** (`drainInputs`): pop matured heads out of each in-order input FIFO
   into its `ready` deque, in arrival order, **returning no credit yet**. Bounded
   by `capacity` (= `source->getMaxSize()`): when the window is full, messages
   stay parked in `source`, so an uncredited input's slot-based backpressure
   still throttles the producer, and a credited input is already bounded by its
   credit pool.
3. **Arbitrate** (`operateVnet` → `selectCandidate` / `grantCandidate`): for each
   output, pick the oldest ready message that routes there and whose downstream
   staging has room, then grant it.

`selectCandidate` scans each input's `ready` deque oldest-first:

- with `enable_ooo_pop == true`, it may skip a head blocked on a congested
  output and take the oldest *eligible* younger message (counted as `holSkips`);
- with `enable_ooo_pop == false`, only the front (oldest) entry is eligible —
  strict head-of-line behavior.

Across inputs it keeps the globally oldest candidate, and it caps grants per
`(input, vnet)` at the per-vnet channel count (`grantsThisCycle`) to model input
crossbar bandwidth — this replaces the buffer's old `max_dequeue_rate` gate.

`grantCandidate` removes the chosen message from the deque (pop-by-position,
invalidation-proof because exactly one candidate is live at a time), calls
`source->returnCredit(now, creditReturnLatency())` — *this is the real
departure* — and enqueues the message (cloning for multicast) into the output
staging buffers.

### Why pop-by-identity is safe here

The deque is the switch's own container, so a grant removes by position and the
loop re-selects after every grant. There is no shared `Handle` into a live heap,
so multiple grants per cycle are safe by construction — none of the
stale-index hazards a buffer-internal OoO API would carry.

---

## 3a. Functional access & the backdoor surface

The `ready` deques hold **in-flight messages** drained from the input buffers
but not yet forwarded. Any container that can hold a message holding line data
is, by definition, a place a functional read/write must reach — so the adapter
inherits the same obligations every Ruby message-holding container has.

### What "functional" means and why in-flight messages must be searched

gem5 touches memory two ways. **Timing/atomic** access is the normal simulated
path. **Functional** access is an out-of-band, *instantaneous* peek/poke that
bypasses timing and the protocol: binary/image loading, `m5 readfile`, GDB
reads, KVM and fast-forward memory sync, SE-mode memory ops, and `memWriteback`
before a checkpoint. The freshest copy of a line may be neither in a cache nor in
memory but **in flight inside a coherence message**, so a functional read must
search every in-flight holder, and a functional write must patch every in-flight
copy.

### The traversal and registration

Functional access is **driven top-down by `RubySystem`**
([system/RubySystem.cc](../system/RubySystem.cc)), which reaches three classes
of in-flight holder: *controller-owned buffers* (SLICC-generated
`functionalReadBuffers`/`functionalWriteBuffers`), *sequencer queues*, and
*network-owned buffers*. The network side is **hand-registered**, not automatic:

- **Internal-link input buffers** are registered into the network's
  `m_int_link_buffers` list in `makeInternalLink`
  ([simple/SimpleNetwork.cc](simple/SimpleNetwork.cc), and `XPNetwork`'s
  override), with the telling comment *"global list of buffers (used for
  functional accesses only)."*
- **Switch-local buffers** are reached by `SimpleNetwork::functionalRead/Write`
  walking every `Switch` and calling its `functionalRead/Write`. The base
  `Switch` scans its `m_port_buffers` (the XP staging buffers).
- **The XP adapter** is the new home, so `XPSwitch` **overrides**
  `functionalRead`/`functionalWrite` to scan every `ReadyQueue::ready` deque and
  then chain to `Switch::` (which covers the staging buffers). For this override
  to dispatch through the `Switch*` the network iterates, the base methods were
  made `virtual`.

This is the registration tax in one sentence: a new container inside a router is
visible to functional access **only** if it is explicitly walked. Miss it and a
functional read returns stale data or a functional write leaves a stale in-flight
copy — a silent divergence that surfaces only on functional paths.

### Checkpoints: drain, don't serialize

Ruby does **not** serialize in-flight messages. `MessageBuffer` has no
`serialize`/`unserialize`, and `RubySystem::serialize` refuses to run unless
`memWriteback()` was called first and the system was drained to a **quiescent
point** — all transactions complete, every buffer empty — then saves only
cache/memory contents as a replayable trace. So **no message ever survives a
checkpoint inside any buffer or adapter.** A new container therefore needs no
serialization code; its only obligation is to *be empty at the drain point*. The
XP `ready` deques drain to empty as the system quiesces (the controllers stop
injecting and the switch forwards everything in flight), so they satisfy this
automatically.

For the CPU-less XP NoC testbench all of this is moot (no real coherence
payloads, no functional reads/writes of meaningful data, no stalls); the
overrides exist so the adapter stays a correct Ruby citizen if reused under a
real protocol.

### The cost of adding a new router container — checklist

| Interface | Required? | Where the XP adapter does it |
| --- | --- | --- |
| `functionalRead(pkt)` / with `WriteMask` | **Yes** if it can hold line data | `XPSwitch::functionalRead` scans `ready`, chains to `Switch::` |
| `functionalWrite(pkt)` | **Yes** if it can hold line data | `XPSwitch::functionalWrite` scans `ready`, chains to `Switch::` |
| Registration into the traversal | **Yes** | `Switch::functionalRead/Write` made `virtual`; `XPSwitch` overrides |
| Single-home invariant (one message, one home) | **Yes** | drain pops from `source` into `ready`; grant erases from `ready` — never double-stored |
| `serialize`/`unserialize` | **No** | Ruby checkpoints replay cache traces, not buffers |
| Drain / quiesce | **Yes** | `ready` empties as the system quiesces |
| Memory backdoor (`memoryPort`) | **No** (network containers) | N/A — only directory/memory controllers own a `memoryPort` |
| Stall/recycle/reanalyze | Only under a real protocol | N/A for the CPU-less testbench |

---

## 4. Backpressure handling (polling)

There is **no** event-driven notification when a credit (or a normal buffer
slot) frees up. Both the credited and the plain bounded-buffer paths rely on the
producer **re-polling on a later cycle**.

A credit return is redeemed lazily — `CreditState::redeemMatured()` moves matured
slots back into `m_credits` the next time the producer queries credit — and it
does not wake the producer. The XPSwitch keeps itself scheduled by treating a
credit-blocked or output-blocked downstream exactly like ordinary backpressure
and re-arming for next cycle ([`XPSwitch.cc`](simple/xp/XPSwitch.cc),
`driveLinks`):

```cpp
DriveResult result = driveOutput(out_port, vnet);
...
// No credit-return callback exists, so a credit-blocked output must
// re-poll next cycle to notice returned credits.
if (result == DriveResult::OutputBlocked ||
    result == DriveResult::CreditBlocked) {
    retry = true;          // wakeup() reschedules at Cycles(1)
}
```

Because the producer's next poll redeems any matured returns before checking the
count, it simply observes the freed credit. The `CreditBlocked`
result is distinguished from `OutputBlocked` only so the switch can count credit
stalls separately (`creditStallCycles`). A message parked in a `ready` deque
because its output staging is full is likewise covered: the staging buffer is
non-empty, so `driveLinks` keeps the switch rescheduling until staging drains.

The cost of polling is that a blocked producer re-evaluates routing and slot
checks every cycle while it waits — wasted work that scales with how long links
stay congested.

---

## 5. Future work: credit-return callback

If profiling shows that per-cycle re-polling of credited buffers is a real
simulator hotspot (a large mesh with many long-congested credited links spending
most cycles re-evaluating blocked outputs), the polling loop can be replaced with
an **event-driven credit-return callback**. The mechanism is intentionally not
implemented today to keep the buffer minimal, but it is a small, self-contained
addition:

1. Give `CreditState` an optional `std::function<void()> m_callback` plus
   `registerCallback` / `unregisterCallback`. To fire it at the right tick the
   lazy redemption must become a scheduled push: arm an `EventFunctionWrapper`
   for the front entry's maturity tick, and from that event redeem and invoke
   the callback *after* `m_credits` has been incremented.
2. Expose `MessageBuffer::registerCreditCallback` /
   `unregisterCreditCallback` thin wrappers that forward to `m_credit`.
3. In the producer, register a callback on each credited downstream buffer that
   schedules the switch's own wakeup, and **stop** treating `CreditBlocked` as a
   reason to re-poll.

Correctness contract for that future change: a producer must use **either**
polling **or** the callback, not neither. The callback is purely a wakeup hint;
it never changes the credit bookkeeping, which stays entirely inside
`CreditState`.

---

## 6. Lifecycle & statistics

- `clear()` resets the credit pool to full and drops all pending returns. There
  is no return event to deschedule.
- `preDumpStats()` flushes the live credit counters into stats before a dump.
- The `CreditState` is a `statistics::Group` named **`credit`** under the buffer,
  exposing: `creditStalls`, `creditReturns`, `availableCreditsStat`,
  `pendingReturnsStat`, and the derived `creditOccupancy`
  (`maxCredits - availableCredits`). All are `nozero`-flagged, so a non-credited
  buffer emits nothing. (`creditReturns` is bumped inside `redeemMatured()`, so a
  non-zero value confirms the lazy pull path is exercised.)
- The XP switch's `xp` stat group exposes `grants`, `holSkips` (OoO skips),
  `creditStallCycles`, `outputBlockedCycles`, `stagingFullCycles`, and
  `linkSends`.

---

## 7. Quick reference: behavior by configuration

| `credits` | Admission | Pop selection | Credit return |
| --- | --- | --- | --- |
| `0` | slot count only | strict-FIFO head-only `dequeue()` | n/a (`returnCredit` is a no-op) |
| `>0` | slots **and** credits | strict-FIFO head-only `dequeue()` | explicit `returnCredit()`, delayed by `credit_return_latency` |

Out-of-order arbitration is the **consumer's** job and is independent of
credits: the XP switch's `enable_ooo_pop` toggles head-of-line skipping over its
own per-input ready container, on either a credited or an uncredited input.
