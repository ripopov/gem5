# Credited / OoO-pop MessageBuffer API

This document describes the **local extensions** to `MessageBuffer` (see the
`<local-addition ...>` markers in [`MessageBuffer.hh`](MessageBuffer.hh)) that
turn an ordinary Ruby message buffer into a **credited link buffer** and/or
allow **out-of-order (oldest-eligible) popping**.

Everything described here is opt-in. When `credits == 0` and
`enable_ooo_pop == False` (the defaults), a `MessageBuffer` behaves exactly like
upstream: infinite/finite slot accounting, strict-FIFO head-only `dequeue()`.

The two features are **orthogonal** and can be enabled independently:

| Feature | Param | What it changes |
| --- | --- | --- |
| Credited flow control | `credits > 0` | Producer-side admission gated by a credit pool; credits returned to the producer some latency *after* the consumer pops. |
| Out-of-order pop | `enable_ooo_pop = True` | Consumer may select the oldest message that satisfies a predicate, not just the head, to avoid head-of-line blocking. |

The canonical consumer of this API is the XP simple-network switch
([`simple/xp/XPSwitch.cc`](simple/xp/XPSwitch.cc)); the behavioral contract is
pinned by [`simple/xp/credited_link_buffer.test.cc`](simple/xp/credited_link_buffer.test.cc).

---

## 1. Parameters ([`MessageBuffer.py`](MessageBuffer.py))

```python
credits               = Param.Unsigned(0, "Credit pool size; 0 disables credits")
credit_return_latency = Param.Cycles(1,
    "Cycles from downstream departure to upstream credit visibility")
enable_ooo_pop        = Param.Bool(False,
    "Allow oldest-eligible selection instead of head-only dequeue")
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

---

## 2. Credit model

A credited buffer starts with `credits` (== `maxCredits()`) available credits.

```
                 enqueue() / spendCredit()        popAt()/dequeue() schedules
producer  ----------------------------------->  buffer  --------------------------> consumer
   ^                  (-1 credit, immediate)                 |
   |                                                         | credit_return_delay later
   +---------------------------------------------------------+
              credit returned (+1) — producer notices it on its next poll
```

- **Spend (immediate):** every `enqueue()` on a credited buffer calls
  `CreditState::spendCredit(delta)`, decrementing the pool by one. It is a fatal
  error (`panic`) to enqueue without an available credit, and `delta` must be
  non-zero (a credited hop always has forward latency). **The producer is
  responsible for checking credit availability before enqueueing** — the buffer
  does not block, it asserts.
- **Return (delayed):** popping a message (`dequeue()` / `popAt()`) schedules the
  freed credit(s) to become visible `credit_return_delay` ticks later, *not*
  immediately. Returns are coalesced per target tick in a `std::map<Tick,
  unsigned>` and processed by a single `EventFunctionWrapper`
  (`Event::Default_Pri - 1`). Until that event fires the credits are counted as
  *pending* (`pendingCreditReturns()`), not available.
- **Notice (polled):** there is currently **no** producer-side notification when
  credits return. The credit-return event only updates `m_credits`; a blocked
  producer learns about the freed credit by polling `hasCredit()` /
  `areNSlotsAvailable()` again on a later cycle. The producer is therefore
  responsible for keeping itself scheduled while it has a pending send (see
  [§5 Backpressure / polling](#5-backpressure-handling-polling)). An
  event-driven alternative is sketched in
  [§6 Future work](#6-future-work-credit-return-callback).

### Producer-side query API

```cpp
bool     isCredited() const;            // credits enabled for this buffer?
bool     hasCredit(unsigned slots = 1); // are >= slots credits available now?
unsigned availableCredits() const;      // credits free right now
unsigned maxCredits() const;            // pool size (== credits param)
Cycles   creditReturnLatency() const;   // configured return latency
unsigned pendingCreditReturns() const;  // credits freed but not yet visible
```

On a **non-credited** buffer these degrade gracefully: `hasCredit()` returns
`true` (always admissible), `availableCredits()/maxCredits()/pendingCreditReturns()`
return `0`, and `creditReturnLatency()` returns `Cycles(0)`.

`areNSlotsAvailable(n, t)` is credit-aware: on a credited buffer it returns
`false` (and records a credit stall) when fewer than `n` credits are available,
*before* doing the normal slot/size check. So a producer that already gates on
`areNSlotsAvailable()` automatically respects credits.

---

## 3. Out-of-order pop model

By default a buffer is strict-FIFO: only the priority-heap head is eligible, and
only when its enqueue time has arrived. With `enable_ooo_pop == True`, the
consumer may instead select the **oldest message that satisfies a predicate**,
skipping a blocked head (head-of-line avoidance, e.g. when the head's route is
congested but a younger message can make progress).

The OoO-pop API is built around a small opaque `Handle`:

```cpp
struct Handle { size_t index; bool valid() const; };
using MessagePredicate = std::function<bool(const Message&)>;
using MessageRank      = std::function<bool(const Message&, const Message&)>;
```

```cpp
// Select a message to pop. With OoO disabled this is head-only;
// with OoO enabled it returns the oldest heap entry that is ready
// (enqueue time arrived), passes `predicate`, and respects the
// per-cycle dequeue-rate limit. Returns an invalid Handle if none.
// An empty predicate short-circuits to the head in O(1); otherwise
// selection is O(n_ready) (see "selection cost" below).
Handle selectEligible(const MessagePredicate &predicate, Tick cur_time) const;

// Generalized selection: among the ready messages that pass `eligible`,
// return the one no other out-ranks under `better` (true => first is
// preferred). `selectEligible` is exactly this with an oldest-first rank.
// An empty `better` returns an arbitrary eligible message. O(n_ready).
// This is the sanctioned hook for QoS pop policies (see below).
Handle selectBest(const MessagePredicate &eligible,
                  const MessageRank &better, Tick cur_time) const;

// Head-only variant (what selectEligible falls back to when OoO is off).
Handle selectHead(const MessagePredicate &predicate, Tick cur_time) const;

// Peek the message a Handle refers to (Handle must be valid).
const MsgPtr& peekAt(Handle handle) const;

// Pop the message a Handle refers to. On a credited buffer this also
// schedules `slots` credit(s) to return after `credit_return_delay` ticks.
Tick popAt(Handle handle, Tick cur_time, Tick credit_return_delay,
           unsigned slots = 1, bool decrement_messages = true);
```

Notes:

- `predicate` may be empty (`MessagePredicate()`); an empty predicate matches
  every message. The XP switch passes a predicate that checks routability and
  downstream staging availability so it never grants a message it cannot forward.
- `selectEligible` honors `max_dequeue_rate` (via `canDequeue`) and message
  readiness exactly like `isReady()` does.
- `handle.index != 0` tells the consumer it skipped the head (the XP switch
  counts these as `holSkips`).

### Handle invalidation (important)

A `Handle` is **just a bare index** into the live priority heap
(`struct Handle { size_t index; }`). It carries no generation/epoch tag, so it
cannot detect that it has gone stale — `valid()` only checks for the sentinel
`invalidMessageIndex`, not whether the index still points at the message you
selected.

Every `popAt`/`dequeue` mutates the heap: removing the head does `pop_heap` +
`pop_back`, and removing an interior element moves the last entry into the hole
and sifts it ([MessageBuffer.cc](MessageBuffer.cc), `dequeueAt` /
`siftHeapEntry`). **A single pop can therefore relocate any number of other
entries**, so the moment you pop, *every other outstanding handle is invalid* —
its index may now refer to a different message or point past the end.

Multiple OoO pops in the **same cycle are fully supported** (the buffer pins the
network-visible size at `m_size_at_cycle_start` until the tick advances, and
`m_dequeues_this_cy` only counts them — there is no per-cycle cap). But they must
be done **one handle at a time**: keep exactly one live handle, and re-select
after each pop. Do **not** select several handles up front and then pop them by
their saved indices.

```cpp
// CORRECT — select → peek → pop → re-select, one handle live at a time.
MessageBuffer::Handle h;
while ((h = buf.selectEligible(pred, t)).valid()) {
    const MsgPtr &m = buf.peekAt(h);   // use the message
    buf.popAt(h, t, delay);            // h is now dead; loop re-selects
}

// WRONG — h2 is invalidated by popping h1; this pops the wrong message.
auto h1 = buf.selectEligible(pred, t);
auto h2 = buf.selectEligible(pred2, t);   // index captured against current heap
buf.popAt(h1, t, delay);                  // heap reorders here
buf.popAt(h2, t, delay);                  // h2.index is now stale -> bug
```

`dequeue(cur_time)` (head-only) and `popAt(...)` share the same underlying
`dequeueAt(index, ...)`. The relationship:

```cpp
// dequeue() is popAt(head) with zero credit-return delay:
Tick dequeue(Tick t, bool dec) {
    Tick d = dequeueAt(0, t, dec);
    if (dec && m_credit) m_credit->scheduleReturn(t, /*delay=*/0, 1);
    return d;
}
```

> Note: plain `dequeue()` returns its credit with delay **0** (next event,
> same tick region), whereas `popAt()` lets the caller pass
> `creditReturnLatency()`. Credited consumers should prefer `popAt()` with the
> real return latency; `dequeue()`'s 0-delay return exists mainly for internal
> reuse (e.g. `stallMessage`, which pops with `decrement_messages = false` and
> therefore returns no credit).

### Design note: selection and removal cost

The messages live in a single binary min-heap (`m_prio_heap`, ordered by
`(enqueue time, counter)`), the same structure upstream uses. An OoO pop is two
operations — *select* a victim, then *remove* it — and both are sub-linear.

**Removal — O(log n).** Removing the **head** is the usual `pop_heap` +
`pop_back`. Removing an **interior** element (what OoO pop does) moves the last
heap element into the hole and then **sifts that element up or down** to restore
the invariant ([MessageBuffer.cc](MessageBuffer.cc), `siftHeapEntry`). An earlier
version rebuilt the whole heap with `std::make_heap` after the swap, which is
O(n); the sift replaces that. Only one sift direction ever does work (if the
moved element rises it cannot also need to sink), so running both is correct and
still O(log n).

**Selection — O(n_ready), via a pruned heap traversal.** `selectEligible` /
`selectBest` do **not** scan the whole heap. They walk only the *matured* (ready)
messages using `forEachReady` ([MessageBuffer.cc](MessageBuffer.cc) /
[MessageBuffer.hh](MessageBuffer.hh)), which exploits a structural invariant:

> **Maturity is a heap prefix.** The heap is a min-heap on
> `(getLastEnqueueTime, counter)`, so every node's enqueue time is `<=` its
> children's. Therefore if a node is immature (`getLastEnqueueTime() > cur_time`)
> its **entire subtree is immature** too. The matured messages form a connected
> sub-heap rooted at index 0.

`forEachReady` is a DFS from the root that **stops descending the moment it hits
an immature node**. It visits exactly the `n_ready` matured entries (touching at
most their immature boundary children), so selection is O(n_ready), not O(n) —
the head fast path makes the no-predicate case O(1) on top of that. A small
inline stack keeps the common shallow-buffer traversal allocation-free.

`selectBest` is an argmax over that ready set under a caller-supplied
`MessageRank`: O(n_ready) to select + O(log n) to remove. `selectEligible` is
just `selectBest` with an oldest-first rank (plus the empty-predicate head
short-circuit). Selecting the single best ready message is inherently at least
O(n_ready), so this is optimal for the operation.

One caveat kept for honesty: **n is small.** These are link buffers sized to
`credits` / `buffer_size` — typically a handful of entries — so at today's depths
the win over the old flat scan is constant-factor. The pruned traversal still
matters: selection is no longer the asymptotic bottleneck, and it pays off if a
buffer is ever sized large. This is **measured** in
[§9](#9-selection-strategy-benchmark-ooo-pop): at shallow depth the spread
between strategies is ~13 ns/pop, while at N=1024 the pruned traversal is ~1.5×
faster than the flat scan on low-maturity selection.

### QoS / custom pop policies: use `selectBest`

When a consumer needs a pop order other than oldest-first — strict priority,
weighted/aged priority, deficit round-robin, etc. — express it as a
`MessageRank` and call `selectBest(eligible, rank, cur_time)`. The ranking runs
inside the single O(n_ready) traversal, so an arbitrary QoS argmax costs the same
as plain oldest-first selection. Any per-class or anti-starvation **state lives
in the consumer** (folded into the rank as a score); the buffer stays policy-free
and keeps the heap as its single source of truth. This is the sanctioned
extension point — reach for the ephemeral snapshot below only when a policy
genuinely needs stateful cross-message bookkeeping that a pairwise rank cannot
express.

### Rejected alternative: a separate "ready" container

A recurring suggestion is to stop selecting over the heap and instead keep a
second, **persistent** container of already-matured messages — e.g. attach a
maturity callback to `enqueue` that moves each message heap → ready-deque as its
arrival time passes, and have `dequeue`/`popAt` operate on that deque. The
intent is random access into a ready set and cheaper removal. It was considered
and **rejected**; the reasoning, in case it comes up again (and the cost is
**measured** in [§9](#9-selection-strategy-benchmark-ooo-pop): the
two-container variant is the slowest arm under mutation and costs +25–50%
memory):

- **It buys no selection win over `forEachReady`.** The predicate the consumer
  cares about is *routability / downstream room*, not maturity, so even with a
  ready-only container you still scan it for the victim. The one thing a
  persistent ready container would save — visiting only matured entries — is
  already what the pruned `forEachReady` traversal does (O(n_ready)), with the
  heap as the sole container. So the persistent version adds machinery for a
  benefit we already have.
- **Removal gets no asymptotic win, and often gets worse.** Arbitrary-position
  erase from a `std::deque`/`std::vector` is O(n) (element shift) — worse than
  the heap's O(log n) `siftHeapEntry`. Switching to a `std::list` for O(1)
  removal throws away the random access that motivated the change.
- **`n` is tiny.** These are credit-/`buffer_size`-sized link buffers — a handful
  of entries — so neither the saved compare nor any amortization argument
  ("don't re-scan matured entries each cycle") pays for the added machinery.
- **It splits the single-queue invariant across the whole class.** A message's
  home becomes "heap if immature, ready-container if matured," and *every* path
  that touches messages must honor that split: `isReady`/`readyTime`,
  `stallMessage`, `reanalyzeMessages`/`reanalyzeAllMessages`, `recycle`,
  `clear`, `functionalAccess`, `getAllMessages`/`print`. That is a large,
  permanent bug surface that does not exist today.
- **Drain, not serialization, is the real obligation — and it is lighter than
  it looks.** Ruby does *not* checkpoint in-flight messages: `MessageBuffer` has
  no `serialize`/`unserialize`, and `RubySystem::serialize`
  ([../system/RubySystem.cc](../system/RubySystem.cc)) requires `memWriteback()`
  and a prior **drain to a quiescent point** — all transactions complete, every
  buffer empty — then saves only cache/memory contents as a replayable trace. So
  no message ever survives a checkpoint inside a buffer, and a dual-container
  buffer cannot "land a message in the wrong container after a round-trip." What
  it *must* do instead is participate in drain correctly: every container has to
  reach empty and be reflected in the controller's quiesce check, or a checkpoint
  blocks (or proceeds with state still live). Smaller than a serialize round-trip,
  but a new drain path to get right.

A victim policy other than oldest-first is already served by `selectBest` with a
custom `MessageRank` (see above) — no new container needed. If a policy ever
needs true random access into the ready set (stateful cross-message bookkeeping a
pairwise rank cannot express), the cheap and safe way is an **ephemeral per-cycle
snapshot** — one O(n_ready) pass that copies the currently
ready `MsgPtr`s (cheap, they are `shared_ptr`s) into a scratch vector the
consumer indexes freely, paired with a `popMsg(MsgPtr)` that removes by
*identity* (invalidation-proof, so multiple pops per cycle are safe by
construction). Because the snapshot is rebuilt and discarded each arbitration, it
touches none of the methods above and is empty at every drain/checkpoint point —
the heap stays the single source of truth. That keeps the same benefit (random access, no stale
handles) without the persistent-container cost. Revisit either option only if a
profile of a large credited mesh shows the OoO pop *and* the selection scan as
real hotspots.

### Alternative design: a consumer-side OoO adapter

A more radical alternative moves OoO out of `MessageBuffer` entirely instead of
optimizing it in place:

1. **Remove** the OoO / random-access feature from `MessageBuffer` (drop
   `Handle`, `selectEligible`/`selectBest`/`selectHead`, interior removal /
   `siftHeapEntry`) — the buffer goes back to upstream-clean FIFO + credits.
2. Add a **separate wrapper/adapter object** that eagerly pops each message from
   the `MessageBuffer` as it matures and stores it in the adapter's own internal
   container. The router then selects/pops *out of the adapter*, where every
   entry is already matured and ordering is the adapter's to define.
3. Add an explicit **`returnCredit()`** method to `MessageBuffer`, decoupling
   "credit returned" from "message popped."
4. When the router pops from the adapter, the **adapter** calls
   `MessageBuffer::returnCredit()`.

This is the "persistent ready container" of the rejected alternative above, but
promoted to a first-class consumer-side object with an explicit credit handshake.
Judged purely on the benchmark it looks like a loss — but the benchmark is the
wrong lens for it. Its real case is **separation of concerns**, developed next,
followed by the honest cost/benefit ledger.

#### The strongest argument: separation of concerns

The benchmark framing above ("is the adapter faster?") undersells the design's
real motivation, which is **responsibility**, not speed. Two concerns are
tangled inside today's `MessageBuffer`:

- **Flow control** — *may a producer admit a message, and when does the slot it
  occupies free up?* This is the credit pool: `spendCredit` on `enqueue`,
  `scheduleReturn` on pop, `hasCredit`/`areNSlotsAvailable` on the producer
  side. It is a small, universal, single-meaning mechanism. It belongs on the
  buffer; every credited link wants exactly this and nothing more.
- **Ready-message selection** — *given the set of arrived messages, which one
  does the consumer take next, and how are they organized while they wait?*
  This is `selectEligible`/`selectBest`, the `Handle`, the predicate, the rank.
  Unlike flow control, this has **many** valid shapes, and the buffer cannot
  know which one its consumer wants.

The selection concern is where the "one container, one policy" assumption stops
paying. A real router does not want a single ordered pool of ready messages; it
wants to *organize* them for the decision it is about to make:

- **Split by routing direction.** A mesh switch routes each ready message toward
  one of N output ports. Holding all of them in one heap means every per-output
  arbiter re-scans the whole ready set filtering for "routes to *my* output."
  An adapter is free to demultiplex on maturity into **per-direction
  sub-queues**, so each output arbiter looks only at its own candidates — the
  routing predicate becomes a container choice instead of a per-cycle filter.
- **Split by QoS class.** Per-class queues, deficit counters, aging state — the
  anti-starvation bookkeeping `selectBest` can only approximate through a
  stateless pairwise rank — live naturally in the adapter as first-class
  structures, not folded into a score.
- **Anything else a future router wants.** Because the adapter owns the ready
  set outright, its internal shape is unconstrained: VC-style per-flow FIFOs,
  a priority tree, a small reorder window. The buffer does not need to grow an
  API for each of these; it only needs to hand the message over once it matures.

Framed this way the split is not "move the heap somewhere else for the same
job." It is: **the buffer owns admission and slot accounting; the consumer owns
how ready work is held and chosen.** That boundary adds flexibility precisely
because the consumer side has many use cases and the buffer side has one.

#### This boundary already exists in the simple network

The SimpleNetwork switch is the proof that gem5 already wants two buffer tiers
with two different jobs — the adapter idea is mostly *naming the second tier and
giving it a clean credit handshake*, not inventing it.

```
   upstream                 Switch
   credited      ┌───────────────────────────────────────┐
   link          │  PerfectSwitch        Throttle(s)      │      downstream
  ───────────▶ [ input buffer ] ──route──▶ [ port buffer ] ──BW/latency──▶ link
   (in-port)      │   (admission,            (staging,     │     (next hop's
                  │    credited)              QoS, drain)   │      input buffer)
                  └───────────────────────────────────────┘
```

Two distinct kinds of buffer, with distinct owners and distinct functional-access
paths (verified in code):

- **Input buffers** — `simple_link->m_buffers`, registered into
  `SimpleNetwork::m_int_link_buffers`
  ([simple/SimpleNetwork.cc](simple/SimpleNetwork.cc), `makeInternalLink`, with
  the telling comment *"global list of buffers (used for functional accesses
  only)"*). These are the upstream-facing admission point — **this is the tier
  that should carry credits.** A producer gates on the input buffer's credit
  pool; the buffer's only job is "is there room, and account for the slot."
- **Port buffers** — `Switch::m_port_buffers`
  ([simple/Switch.cc](simple/Switch.cc)), the per-output intermediate staging
  that `PerfectSwitch` routes *into* and `Throttle` drains *out of* under
  bandwidth/latency limits. **This is exactly the consumer-side ready container**
  the adapter generalizes: it already holds post-routing messages, already has a
  natural owner (the `Throttle`), and is already the place QoS/bandwidth policy
  is enforced.

That structure makes the credit-return timing fall out cleanly, and it answers
"when, precisely, is a credit returned?" better than today's pop-couples-return:

> Return the input-buffer credit when the **Throttle actually moves the message
> out of the port buffer** onto the downstream link — i.e. on real departure,
> not on the intermediate routing hop into the port buffer.

That is the honest definition of "the slot is free again": the message has left
the switch, not merely shuffled between its internal stages. It requires exactly
the `returnCredit()` decoupling (steps 3–4): the routing hop into the port buffer
pops the input buffer **without** returning credit
(`popAt(..., decrement_messages = false)` is already this shape — see
[stallMessage](MessageBuffer.cc)), and the Throttle calls
`inputBuffer.returnCredit()` when it dequeues from the port buffer. The credit
pool then bounds `input_buffer.occupancy + port_buffer.occupancy` —
admitted-but-not-yet-departed — which is the quantity a credited link is
*supposed* to bound. And the QoS policy lives entirely in the port buffer, where
the simple network already puts it, with no policy state on the credited input
buffer at all.

It is a legitimate option with a real tradeoff, not a clear win:

**What it buys**

- **Clean separation of concerns (the main point).** The credited buffer owns
  flow control only; the consumer owns how ready work is organized and chosen.
  The simple-network two-tier structure above stops being an accident of the
  implementation and becomes the explicit contract: input buffer = credits,
  port buffer = QoS/selection.
- **`MessageBuffer` returns to upstream-clean.** Steps 1+3 shrink the
  local-addition surface to just credits plus `returnCredit()`. The whole
  `Handle`-invalidation hazard (above) disappears.
- **Pop-by-identity in the adapter is invalidation-proof**, so multiple pops per
  cycle are safe by construction — no "one live handle at a time" rule.
- **The credit decoupling (steps 3+4) is sound and arguably better.** As long as
  the adapter drains **without** returning credit and returns it only on
  real downstream departure, the credit pool correctly bounds
  `buffer.size + adapter.size` (admitted-but-not-yet-departed). An explicit
  `returnCredit()` separates "left the buffer" from "actually departed
  downstream," which is more honest than today's pop-couples-return — and lines
  up exactly with "return credit when the Throttle drains the port buffer."
- The adapter's container has **no enqueue-time ordering constraint** (everything
  in it is matured), so QoS structures (intrusive list, per-class queues,
  per-direction sub-queues) are free of the heap's baggage.

**What it costs**

- **No performance win — measured.** This is the
  [§9](#9-selection-strategy-benchmark-ooo-pop) two-container arm (the slowest
  under the mutation-heavy pop workload and the 1M-transaction steady state),
  made persistent and cross-object. It is in fact *more* work: every message is
  removed **twice** (head-popped from the heap on drain, then removed from the
  adapter on router-pop) versus **once** today (selected straight out of the
  heap), plus the container insert and the extra `shared_ptr` storage (the same
  +25–50% memory). If the motivation is speed, this is the wrong direction —
  but separation of concerns, not speed, is the reason to do it.
- **Eager draining needs precise wakeups.** The adapter must be the buffer's
  `Consumer` and drain newly-matured heads on each maturity event — doable
  (`enqueue` already schedules `scheduleEventAbsolute(arrival_time)`), but it is
  now two objects with coupled scheduling.
- **The functional-access / checkpoint obligation is the real tax.** This is
  large enough that it gets its own treatment in
  [Functional access & the backdoor surface](#functional-access--the-backdoor-surface-what-a-new-container-must-implement)
  below. In short: a cross-object adapter that holds drained-but-not-departed
  messages is a *new home for in-flight data* that Ruby's functional path does
  not know about, so it must be explicitly wired into the traversal and (for a
  general protocol) into stall/recycle/reanalyze as well. Harmless for the
  CPU-less XP NoC testbench (no functional coherence, no stalls); a real cost
  for general `MessageBuffer` reuse.

**When to pick it.** The deciding question is *does anything other than the XP
switch use OoO pop, and does it run a real coherence protocol?*
- **Single consumer, no functional coherence (the testbench):** the adapter is
  an attractive refactor — push all OoO/QoS/per-direction policy into the
  switch's own adapter, keep `MessageBuffer` pristine and credit-only, and accept
  the small perf/serialize cost the testbench never exercises. The
  separation-of-concerns win is real and the functional tax is zero here.
- **OoO as a general `MessageBuffer` capability under a real protocol:** the
  adapter must re-implement the whole functional/stall/serialize surface in
  §3 to stay correct; keeping selection in place is strictly less code to get
  right.

**Recommended middle path.** Adopt step 3 (and the spirit of step 4) *without*
necessarily committing to steps 1–2 buffer-wide: add `returnCredit()` and let
consumers `popAt(..., decrement_messages = false)` then return the credit on
real departure. That captures the cleaner credit handshake — and the
input-buffer-credited / port-buffer-QoS split — at near-zero risk, keeps the
in-place O(n_ready) selection the benchmark prefers as the *default*, and leaves
functional access / stalling / serialization on the single heap. A future
per-consumer adapter then becomes an **opt-in** choice for a specific router that
wants per-direction or per-class organization, layered on the same
`returnCredit()` primitive — rather than a buffer-wide commitment that taxes
every `MessageBuffer` user.

### Functional access & the backdoor surface: what a new container must implement

Both dual-container ideas above are constrained by **functional access**, and
more generally any new message-holding container added inside a router/switch
inherits a list of obligations to stay a "fully featured" Ruby citizen
(functional reads/writes, checkpoints, draining). This section pins down what
those interfaces are, how the existing traversal finds buffers, and what it costs
to add a new one — derived from the actual code paths, with `file:line`
references so it can be used as an implementation checklist.

#### What "functional" means and why in-flight messages must be searched

gem5 touches memory two ways. **Timing/atomic** access is the normal simulated
path — modeled latency, drives the coherence protocol. **Functional** access is
an out-of-band, *instantaneous* peek/poke that bypasses timing and the protocol:
binary/image loading, `m5 readfile`, GDB reads, KVM and fast-forward memory sync,
SE-mode memory ops, and `memWriteback` before a checkpoint.

The wrinkle is coherence: the freshest copy of a line may be neither in a cache
nor in memory but **in flight inside a coherence message** — a WriteBack/Data
message carrying dirty bytes between controllers. So a functional **read** must
search everywhere a valid copy could be, *including in-flight messages*, and a
functional **write** must patch every in-flight copy or a stale value is later
delivered. Any container you add that can hold a message holding line data is, by
definition, one of those places.

#### The traversal, top to bottom

Functional access is **driven top-down by `RubySystem`**, which knows every
controller and every network, and dispatched down to each message-holding object.
The order and the participants
([system/RubySystem.cc](../system/RubySystem.cc)):

1. **`RubySystem::functionalRead(pkt)`** (`RubySystem.cc:508`). It first ranks
   controllers by coherence permission (`Read_Write`, `Read_Only`,
   `Backing_Store`, `Busy`, `Maybe_Stale`) and reads stable cache/memory state
   directly: `ctrl_rw->functionalRead(line_address, pkt)` etc.
   (`RubySystem.cc:601,621,624`). Only if the freshest copy is *not* in a stable
   cache (a `Busy`/`Maybe_Stale` line — i.e. a transaction is mid-flight) does it
   fall through to the in-flight searches:
   - **controller message queues:** `cntrl->functionalReadBuffers(pkt)`
     (`RubySystem.cc:635`),
   - then **network in-flight buffers:** `network->functionalRead(pkt)`
     (`RubySystem.cc:642`).
   First hit wins; a simple read returns `true` and stops.
2. **`RubySystem::functionalWrite(pkt)`** (`RubySystem.cc:759`) has no "first hit
   wins" — it must patch **every** copy, so it unconditionally calls, for every
   controller in the line's network: `functionalWriteBuffers(pkt)` +
   `functionalWrite(line_addr, pkt)` (cache state) + the sequencers'
   `functionalWrite`, then every `network->functionalWrite(pkt)`
   (`RubySystem.cc:775-797`). It returns the **count** of copies written.
3. **Partial reads** (`RubySystem.cc:658`) exist for protocols whose messages
   carry only a byte subset (`protocolInfo->getPartialFuncReads()`): the same
   walk but accumulating into a `WriteMask` until every byte is covered.

So there are exactly **three classes of in-flight holder** the system knows how
to reach: *controller-owned buffers*, *sequencer queues*, and *network-owned
buffers*. A new container must be reachable through one of these, or it is
invisible.

#### How buffers get *registered* (the part you must not forget)

There is no global "list of all MessageBuffers." Reachability is hand-maintained
at two layers:

- **Controllers (SLICC-generated).** `functionalReadBuffers`/
  `functionalWriteBuffers` are **code-generated** by the SLICC compiler
  ([slicc/symbols/StateMachine.py:1304-1366](../../slicc/symbols/StateMachine.py)):
  the generator loops over *every object whose type `isBuffer`* in the state
  machine and emits one `if ($vid->functionalRead(pkt)) return true;` /
  `num_functional_writes += $vid->functionalWrite(pkt);` per buffer. The payoff:
  **any `MessageBuffer` declared as a SLICC controller member is covered
  automatically** — you get functional access for free by declaring it in the
  `.sm`. The flip side: a container that is *not* a SLICC `MessageBuffer` member
  (a hand-written C++ container in a controller) is **not** generated into these
  functions and must be added by hand.
- **Networks (hand-registered).** The simple network keeps a **manual registry**:
  `SimpleNetwork::m_int_link_buffers`, populated in `makeInternalLink` with the
  blunt comment *"Maintain a global list of buffers (used for functional accesses
  only)"* ([simple/SimpleNetwork.cc:158-160](simple/SimpleNetwork.cc)).
  `SimpleNetwork::functionalRead/Write` (`SimpleNetwork.cc:175-218`) walks two
  things: every `Switch` (`it.second->functionalRead(pkt)`) **and** every buffer
  in `m_int_link_buffers`. Each `Switch::functionalRead`
  ([simple/Switch.cc:144-173](simple/Switch.cc)) in turn walks its own
  `m_port_buffers`. **Nothing is automatic here** — a buffer that is neither in
  `m_int_link_buffers` nor scanned by a `Switch` is unreachable. (Garnet mirrors
  this manually too: `GarnetNetwork::functionalRead/Write` walks routers, NIs,
  links, and bridges.)

This is the registration tax in one sentence: **controller buffers are covered by
declaring them in SLICC; network buffers are covered only by adding them to a
hand-maintained traversal.** A new container inside a router falls into the
second category.

#### Inside a single buffer: the single-home invariant

`MessageBuffer::functionalAccess` ([MessageBuffer.cc:1035-1074](MessageBuffer.cc))
is the leaf of the whole walk:

```cpp
// reads return at the first message that has the data (return 1);
// writes update every matching message (return the count).
for (msg : m_prio_heap)       msg->functionalRead/Write(pkt);  // in-flight
for (msg : m_stall_msg_map)   msg->functionalRead/Write(pkt);  // stalled
```

(`functionalRead(pkt)` is `functionalAccess(pkt, true, nullptr) == 1`;
`functionalWrite(pkt)` is `functionalAccess(pkt, false, nullptr)` — the count;
see [MessageBuffer.hh](MessageBuffer.hh).) It bottoms out in the **protocol
message's** own `Message::functionalRead/Write`
([slicc_interface/Message.hh](../slicc_interface/Message.hh)), which each
generated message subclass implements to match its address and copy its payload.

Two properties matter:

- It scans **both** homes the buffer owns — the priority heap **and** the stall
  map (`m_stall_msg_map`, where `stallMessage` parks a message that cannot yet be
  handled — [MessageBuffer.cc:801-812](MessageBuffer.cc)).
- It **ignores maturity**: a message carries its payload regardless of enqueue
  time, so functional access visits the whole heap (immature entries included),
  not just the ready set. This is the one operation that legitimately must touch
  all `n`, not `n_ready`.

The invariant that makes the whole system correct: **each in-flight message has
exactly one home — some buffer's heap or stall map — and `functionalAccess`
covers both.** The OoO selection design preserves this precisely because it never
moves a message off the heap until it is popped for good.

#### Checkpoints: drain, don't serialize

A crucial simplification — and the reason "add a new container" is cheaper than
it first appears: **Ruby does not serialize in-flight messages.**

- `MessageBuffer` implements **no** `serialize`/`unserialize` (confirmed: there is
  no `Serializable` machinery on it — [MessageBuffer.hh](MessageBuffer.hh)).
- `RubySystem::serialize` ([RubySystem.cc:333-358](../system/RubySystem.cc))
  refuses to run unless `memWriteback()` was called first
  (`fatal("Call memWriteback() before serialize()...")`, `RubySystem.cc:344-346`).
  `memWriteback()` (`RubySystem.cc:224`) flushes cache contents into a
  `CacheRecorder` trace; the checkpoint stores **only** the cache-block size and
  that replayable trace (`RubySystem.cc:338-357`). On restore,
  `RubySystem::unserialize` (`RubySystem.cc:403`) reloads the trace and the
  controllers warm their caches by **re-issuing requests** — regenerating any
  in-flight traffic from scratch rather than restoring buffer contents.

The corollary: a checkpoint is only taken at a **quiescent point** — the
simulator is drained, all transactions complete, every buffer empty. So **no
message ever survives a checkpoint inside any buffer or adapter.** A new
container therefore needs **no serialization code at all**; its obligation is
purely to *be empty at the drain point*, so that `memWriteback`'s recorded cache
state is the whole story.

#### Draining: the obligation a new container actually has

Because checkpointing relies on quiescence, the real obligation is **drain**, and
it is enforced at the controller/sequencer level, not the buffer level:

- Drain in Ruby is the standard gem5 `Drainable` protocol (`DrainState`,
  `drain()`, `signalDrainDone()`). The `Sequencer` participates explicitly:
  `testDrainComplete()` and `drainState() != DrainState::Draining` guards
  ([system/Sequencer.cc:229,316,822](../system/Sequencer.cc)) — a sequencer
  reports drained only when it has no outstanding requests.
- `MessageBuffer` has no `drain()` of its own; it is drained *transitively* —
  the controllers stop injecting and the network delivers everything in flight,
  so buffers empty out as the system quiesces. A controller's "am I quiescent?"
  check is what gates the global drain.

For a **new container** the rule follows directly: it must **reach empty** as the
system quiesces and that emptiness must be **reflected in whatever quiesce/drain
check its owner reports**. If the container can hold a message while the owning
controller/switch claims to be drained, a checkpoint can be taken with live state
that `memWriteback` did not record — a silent corruption on restore. This is a
new drain path to get right, but (per §"Checkpoints" above) it is strictly
*lighter* than a serialize/unserialize round-trip: reach empty, report it, done.

#### Memory backdoor (a different, narrower mechanism)

Distinct from message functional access is the **memory backdoor**: the path by
which a controller reaches the backing store directly, bypassing the network.
`AbstractController::functionalMemoryRead/Write`
([slicc_interface/AbstractController.cc:358-373](../slicc_interface/AbstractController.cc))
first checks the controller's own memory request queue, then issues
`memoryPort.sendFunctional(pkt)` straight to memory. This is relevant only to
controllers that own a `memoryPort` (directories / memory controllers); a
network-internal container does **not** participate in it. (gem5 also has a
`MemBackdoor` fast-path object for caches to map a memory region for direct
access, but that is a CPU-side/memory-side mechanism, not a Ruby-network buffer
concern.) Listed here only so the full backdoor surface is on one page: a new
router container touches *message* functional access, not the *memory* backdoor.

#### The cost of adding a new container — checklist

Putting it together, here is the full bill for adding a message-holding container
inside a router/switch and keeping it a fully-featured Ruby citizen:

| Interface | Required? | What to do | Where the existing code does it |
| --- | --- | --- | --- |
| `functionalRead(pkt)` / with `WriteMask` | **Yes** if it can hold line data | Scan held messages, call `msg->functionalRead`; return on first hit | [MessageBuffer.cc:1035](MessageBuffer.cc) |
| `functionalWrite(pkt)` | **Yes** if it can hold line data | Scan **all** held messages, patch each, return count | [MessageBuffer.cc:1050](MessageBuffer.cc) |
| **Registration into the traversal** | **Yes** | Add to the network's hand-maintained walk (`m_int_link_buffers`-style list, or a `Switch::functionalRead` scan); SLICC buffers get this free, custom containers do **not** | [SimpleNetwork.cc:158](simple/SimpleNetwork.cc), [Switch.cc:144](simple/Switch.cc), [StateMachine.py:1304](../../slicc/symbols/StateMachine.py) |
| Single-home invariant | **Yes** | A message must have exactly one home; don't double-store, don't leave a copy behind on pop | invariant, [MessageBuffer.cc](MessageBuffer.cc) |
| `serialize`/`unserialize` | **No** | Ruby checkpoints replay cache traces, not buffers | absent by design, [RubySystem.cc:333](../system/RubySystem.cc) |
| Drain / quiesce | **Yes** | Reach empty at quiescence; reflect emptiness in the owner's drain check | [Sequencer.cc:822](../system/Sequencer.cc) |
| Memory backdoor (`memoryPort`) | **No** (network containers) | N/A unless it owns backing store | [AbstractController.cc:358](../slicc_interface/AbstractController.cc) |
| Stall/recycle/reanalyze coverage | Only under a real protocol | If the container can hold a message that later needs `stallMessage`/`reanalyzeMessages`/`recycle`, those paths must find it too | [MessageBuffer.cc:801](MessageBuffer.cc) |

Mapping that back to the designs in this document:

- An **in-class second container** (heap + ready-deque inside `MessageBuffer`) is
  the easy case: extend `functionalAccess`'s loop to scan it too, exactly as it
  already scans the stall map. Registration, drain, and single-home are all
  inherited because the object is still one `MessageBuffer`.
- A **cross-object consumer-side adapter** is the hard case: it is a *new
  SimObject-ish home* for in-flight data. It must implement
  `functionalRead`/`functionalWrite`, be **explicitly added** to the network's
  hand-maintained traversal (it gets nothing from SLICC), reach empty at drain,
  and — under a real protocol — be visible to stall/recycle/reanalyze. Miss the
  registration step and a functional read returns stale data or a functional
  write leaves a stale in-flight copy: a **silent** divergence that surfaces only
  on functional paths (checkpoint restore, KVM switch, debugger), the hardest to
  test.

For the CPU-less XP NoC testbench all of this is moot (no real coherence
payloads, no functional reads/writes of meaningful data, no stalls); the
checklist is a correctness concern only when OoO/adapter/per-direction containers
are reused under a real protocol.

---

## 4. Canonical consumer flow (XPSwitch)

Putting both features together, the switch's per-(vnet, output) arbitration loop
([`XPSwitch.cc`](simple/xp/XPSwitch.cc)) looks like:

```cpp
// 1. SELECT: oldest input message that routes to this output and whose
//    downstream staging has room. OoO-pop lets a blocked head be skipped.
auto predicate = [&](const Message &m) {
    return routesToOutput(m, vnet, output, routes) &&
           stagingAvailable(routes, vnet, current_time);
};
MessageBuffer::Handle h = buffer->selectEligible(predicate, current_time);
if (!h.valid()) continue;                 // nothing eligible from this input

// 2. PEEK + arbitrate across inputs by message age.
MsgPtr msg = buffer->peekAt(h);
// ... pick the globally-oldest candidate ...

// 3. GRANT/POP: return the credit after the real link latency.
if (buffer->isCredited()) {
    Tick delay = cyclesToTicks(buffer->creditReturnLatency());
    buffer->popAt(h, current_time, delay);
} else {
    buffer->popAt(h, current_time, /*credit_return_delay=*/0);
}
```

And the credit-gated **send** side (downstream admission):

```cpp
if (!downstream->areNSlotsAvailable(1, current_time)) {
    if (downstream->isCredited() && !downstream->hasCredit()) {
        // out of credits — re-poll next cycle (see §5)
        return DriveResult::CreditBlocked;
    }
    return DriveResult::OutputBlocked;       // bounded buffer, no slot
}
...
downstream->enqueue(msg_ptr, current_time, cyclesToTicks(linkLatency), ...);
```

---

## 5. Backpressure handling (polling)

There is **no** event-driven notification when a credit (or a normal buffer
slot) frees up. Both the credited and the plain bounded-buffer paths rely on the
producer **re-polling on a later cycle**.

The credit-return event ([MessageBuffer.cc](MessageBuffer.cc),
`CreditState::processReturn`) only updates the credit count — it does not wake
the producer. So a producer that finds itself blocked must keep itself scheduled
until it can make progress. The XPSwitch does this by treating a credit-blocked
output exactly like an output-blocked one and re-arming for next cycle
([`XPSwitch.cc`](simple/xp/XPSwitch.cc), `driveLinks`):

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

This matches how upstream `SimpleNetwork`'s `PerfectSwitch` already relieves
ordinary backpressure (`scheduleEvent(Cycles(1))` when a downstream buffer is
full). Correctness does not depend on any notification: because the credit count
is updated unconditionally by the return event, the producer's next poll simply
observes the freed credit.

The `CreditBlocked` result is still distinguished from `OutputBlocked` only so
the switch can count credit stalls separately (`creditStallCycles`).

The cost of polling is that a blocked producer re-evaluates routing and slot
checks every cycle while it is waiting — wasted work that scales with how long
links stay congested (the `credit_return_latency` round-trip can be many cycles).

---

## 6. Future work: credit-return callback

If profiling shows that per-cycle re-polling of credited buffers is a real
simulator hotspot (a large mesh with many long-congested credited links spending
most cycles re-evaluating blocked outputs), the polling loop can be replaced with
an **event-driven credit-return callback**. The mechanism is intentionally not
implemented today to keep the buffer minimal, but it is a small, self-contained
addition:

1. Give `CreditState` an optional `std::function<void()> m_callback` plus
   `registerCallback` / `unregisterCallback`, and invoke it at the end of
   `processReturn()` *after* `m_credits` has been incremented (i.e. only when
   credits actually became available this event).
2. Expose `MessageBuffer::registerCreditCallback` /
   `unregisterCreditCallback` thin wrappers that forward to `m_credit` (and
   no-op when the buffer is not credited).
3. In the producer (e.g. `XPSwitch::addXPOutPort`), register a callback on each
   credited downstream buffer that schedules the switch's own wakeup
   (`scheduleEvent(Cycles(0))`), and **stop** treating `CreditBlocked` as a
   reason to re-poll — the producer goes idle while blocked and is woken exactly
   when a credit returns.

Correctness contract for that future change: a producer must use **either**
polling **or** the callback, not neither. If a producer stops self-rescheduling
on `CreditBlocked`, it *must* have registered a callback, otherwise no event will
ever bring it back and the link deadlocks. The callback is purely a wakeup hint;
it never changes the credit bookkeeping, which stays entirely inside
`CreditState`.

This was prototyped and removed; restore it only with a benchmark showing the
polling overhead it eliminates.

---

## 7. Lifecycle & statistics

- `clear()` resets the credit pool to full, drops all pending returns, and
  deschedules the return event.
- `preDumpStats()` flushes the live credit counters into stats before a dump.
- The `CreditState` is a `statistics::Group` named **`credit`** under the buffer,
  exposing: `creditStalls`, `creditReturns`, `creditReturnEventCount`,
  `availableCreditsStat`, `pendingReturnsStat`, and the derived `creditOccupancy`
  (`maxCredits - availableCredits`). All are `nozero`-flagged, so a non-credited
  buffer emits nothing.

---

## 8. Quick reference: behavior by configuration

| `credits` | `enable_ooo_pop` | Admission | Pop selection | Credit return |
| --- | --- | --- | --- | --- |
| `0` | `False` | slot count only | head only (`dequeue`/`popAt(head)`) | n/a |
| `0` | `True` | slot count only | oldest predicate-eligible (`selectEligible`) | n/a |
| `>0` | `False` | slots **and** credits | head only | delayed by `credit_return_latency` |
| `>0` | `True` | slots **and** credits | oldest predicate-eligible | delayed by `credit_return_latency` |

---

## 9. Selection-strategy benchmark (OoO pop)

The selection-cost design in [§3](#3-out-of-order-pop-model) makes two claims:
the pruned `forEachReady` traversal is the right way to select, and a separate
persistent "ready" container ([§3, rejected alternative](#rejected-alternative-a-separate-ready-container))
would not pay off. Both were **measured**, not just argued. Three interchangeable
selection strategies were implemented behind the **same public API** (all pass
the same 24 gtests in
[`credited_link_buffer.test.cc`](simple/xp/credited_link_buffer.test.cc)) and run
through one shared microbenchmark,
[`message_buffer_ooo_bench.test`](simple/xp/message_buffer_ooo_bench.cc).

Full write-up and raw CSVs:
[`MessageBufferOooSelectionBenchmark.md`](MessageBufferOooSelectionBenchmark.md)
and [`ooo_bench_data/`](ooo_bench_data/).

### The three arms

| Arm | Branch | Selection | Extra memory |
| --- | --- | --- | --- |
| **V1 pruned DFS** *(production, this branch)* | `message-buffer-credited-mode` | `forEachReady` DFS, prunes immature subtrees — **O(n_ready)** | none (heap only) |
| **V2 flat scan** | `bench/ooo-v2` | flat **O(n)** scan over the whole heap (historical `findReady`) | none (heap only) |
| **V3 two-container cache** | `bench/ooo-v3` | heap **+** a random-access vector caching matured heap indices; rebuilt lazily, **invalidated on every mutation** | heap + index cache |

### Benchmark

Two workloads. The **sweep** fills a buffer at occupancy `N ∈ {1…1024}` ×
matured fraction `{30%, 70%, 100%}` and measures: `ns_per_select` (repeated
selection on a *static* buffer), `ns_per_pop` (a `select + popAt` *drain*, i.e.
the heap is mutated every step — the real OoO-pop op), and `selector_bytes`
(active footprint). The **steady-state** workload models the actual simulator
regime: hold the buffer at a *shallow* depth and run **1M
enqueue→select→pop transactions**.

Absolute ns are machine-specific; the **ratios between arms** are the result.
Numbers below are from one idle-host session (see the report to reproduce).

### Headline results

**Selection vs. pop at N=1024** (ns, lower better):

| metric | V1 pruned DFS | V2 flat scan | V3 cache |
| --- | --: | --: | --: |
| select, 30% matured | 703 | 856 | **427** |
| select, 100% matured | 2174 | 1430 | **1424** |
| **pop (drain), 30% matured** | **484** | 708 | 679 |
| **pop (drain), 70% matured** | 963 | **835** | 1411 |
| memory @ 100% (bytes) | **16384** | **16384** | 24576 |

**Steady-state, shallow depth, 1M transactions** (ns/transaction, lower better):

| depth | V1 pruned DFS | V2 flat scan | V3 cache |
| --: | --: | --: | --: |
| 10 | 79.3 | **73.6** | 86.6 |
| 32 | 128.7 | **112.4** | 147.6 |

### What it confirms (ties back to §3)

1. **The two-container cache (V3) is a net loss** — slowest pops in the drain,
   slowest in the 1M-transaction steady state, and **+25–50% memory**. Cache
   invalidation on every pop erases any reuse. This is the
   [rejected-alternative](#rejected-alternative-a-separate-ready-container)
   argument, now measured: a separate ready container buys no selection win and
   costs memory and bookkeeping.
2. **The pruned DFS (V1) is the right default.** It wins the mutation-heavy drain
   at low/partial maturity — the backpressured-link regime — with zero extra
   memory, and stays robust as depth grows.
3. **The "n is small → constant-factor" caveat holds.** At realistic shallow
   depth the spread between strategies is only ~13 ns/transaction. The flat scan
   (V2) is even marginally *faster* than V1 when the buffer is tiny or fully
   matured (cache-resident, no DFS bookkeeping), but loses at large N / low
   maturity where it scans slots it cannot use. V1 trades that small shallow-depth
   lead for robustness across the whole depth range.

### Reproduce

```sh
for br in message-buffer-credited-mode bench/ooo-v2 bench/ooo-v3; do
  git switch $br
  scons --ignore-style \
    build/RISCV/mem/ruby/network/simple/message_buffer_ooo_bench.test.opt -j$(nproc)
  ./util/run_with_timeout.sh \
    ./build/RISCV/mem/ruby/network/simple/message_buffer_ooo_bench.test.opt \
    2>/dev/null | grep -E '^MBBENCH|^MBSTEADY'
done
```
