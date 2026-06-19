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
```

```cpp
// Select a message to pop. With OoO disabled this is head-only;
// with OoO enabled it returns the oldest heap entry that is ready
// (enqueue time arrived), passes `predicate`, and respects the
// per-cycle dequeue-rate limit. Returns an invalid Handle if none.
Handle selectEligible(const MessagePredicate &predicate, Tick cur_time) const;

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

- A `Handle` is only valid within the cycle it was produced — it indexes into the
  live priority heap, which is mutated by any pop. **Select → peek → pop within
  the same cycle**; do not cache a handle across dequeues.
- `predicate` may be empty (`MessagePredicate()`); an empty predicate matches
  every message. The XP switch passes a predicate that checks routability and
  downstream staging availability so it never grants a message it cannot forward.
- `selectEligible` honors `max_dequeue_rate` (via `canDequeue`) and message
  readiness exactly like `isReady()` does.
- `handle.index != 0` tells the consumer it skipped the head (the XP switch
  counts these as `holSkips`).

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
