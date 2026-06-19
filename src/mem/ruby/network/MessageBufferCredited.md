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
buffer is ever sized large.

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
and **rejected**; the reasoning, in case it comes up again:

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
- **Serialization is the worst of it.** Checkpoints would have to record which
  container each message lives in (plus any pending maturity state) and restore
  it faithfully. Today `serialize` just dumps messages with their times and the
  heap rebuilds passively on `unserialize`; a dual-container buffer that lands a
  message in the wrong container after a checkpoint round-trip is a silent
  divergence.

A victim policy other than oldest-first is already served by `selectBest` with a
custom `MessageRank` (see above) — no new container needed. If a policy ever
needs true random access into the ready set (stateful cross-message bookkeeping a
pairwise rank cannot express), the cheap and safe way is an **ephemeral per-cycle
snapshot** — one O(n_ready) pass that copies the currently
ready `MsgPtr`s (cheap, they are `shared_ptr`s) into a scratch vector the
consumer indexes freely, paired with a `popMsg(MsgPtr)` that removes by
*identity* (invalidation-proof, so multiple pops per cycle are safe by
construction). Because the snapshot is rebuilt and discarded each arbitration, it
touches none of the methods above and nothing in serialization — the heap stays
the single source of truth. That keeps the same benefit (random access, no stale
handles) without the persistent-container cost. Revisit either option only if a
profile of a large credited mesh shows the OoO pop *and* the selection scan as
real hotspots.

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
