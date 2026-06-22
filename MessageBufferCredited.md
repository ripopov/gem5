# Credited MessageBuffer — Final Spec

Status: **API specification.** This document pins the public-API surface that
credited mode adds to `MessageBuffer`. Design rationale (the consumer-side
out-of-order adapter, functional access, polling vs. callback) lives in later
sections to be filled in; this turn defines **the API contract only**.

Credited mode is **opt-in and backward compatible**. With `credits == 0` (the
default) a `MessageBuffer` behaves exactly as upstream: infinite/finite slot
accounting, strict-FIFO head-only `dequeue()`, and `returnCredit()` is an inert
no-op. The buffer is *always* strict-FIFO; credited mode adds a **flow-control
layer** (a credit pool with delayed, decoupled credit return) and **nothing
else**. Out-of-order selection is a consumer policy and is not part of this API.

The first upstreamed version is deliberately **minimal**: the buffer is a pure
credit counter. It exposes only the single mutator needed to return a credit and
folds the credit check into the existing admission gate. Everything that is only
useful for inspection, for the OoO adapter, or for stats is left out and can be
added later without changing this surface (see §9).

### Design goal: negligible overhead when credits are disabled

`MessageBuffer` is on Ruby's hottest path, so a primary requirement is that the
**non-credited path (`credits == 0`) pays essentially nothing** for the feature's
existence. The mechanism is built to meet this:

- The credit state is a `std::unique_ptr<CreditState> m_credit` that is **`nullptr`
  when `credits == 0`** — no `CreditState` object is allocated, so there is no
  extra memory and no construction cost for a non-credited buffer.
- Every credit-aware site (`enqueue`, `areNSlotsAvailable`, `dequeue`'s
  credit-neutrality, `clear`, `returnCredit`, `isCredited`) gates its credit
  logic behind a single `if (m_credit)` **null-pointer test** — one
  highly-predictable branch, no virtual dispatch, no map lookups, no per-call
  allocation on the non-credited path.
- No new always-on bookkeeping: there are no credit stats (§5/§9), and no
  scheduled events anywhere (redemption is lazy), so a non-credited buffer
  behaves byte-for-byte as upstream apart from those branch-predicted null checks.

Anything that would add unconditional cost to the common path is therefore kept
out of the buffer (latency config, OoO selection, stats, inspection getters) —
which is the same reasoning that drives the minimal surface above.

---

## 1. API at a glance

| Kind | Symbol | Status |
| --- | --- | --- |
| Param | `credits` | **added** |
| Method | `~MessageBuffer()` | **added** (out-of-line) |
| Method | `bool isCredited() const` | **added** |
| Method | `void returnCredit(Tick cur_time, Tick credit_return_delay, unsigned slots = 1)` | **added** |
| Method | `bool areNSlotsAvailable(unsigned, Tick)` | **modified** (credit-aware) |
| Method | `void enqueue(...)` | **modified** (spends a credit) |
| Method | `Tick dequeue(...)` | **modified semantics** (credit-neutral) |
| Method | `void clear()` | **modified** (resets credit pool) |
| Constructor | `MessageBuffer(const Params&)` | **modified** (builds credit state, validates) |
| Internal | `class CreditState`, `m_credit`, `bool canDequeue(Tick) const` | **added** (private) |

That is the entire public surface: **one new param, two new methods
(`isCredited`, `returnCredit`), one new destructor, four modified methods.** The
only query getter is `isCredited()`; no `hasCredit`, `availableCredits`,
`maxCredits`, `creditReturnLatency`, or `getMaxSize`, and no stats — see §9.

---

## 2. Parameters ([`MessageBuffer.py`](src/mem/ruby/network/MessageBuffer.py))

```python
credits = Param.Unsigned(0, "Credit pool size; 0 disables credits")
```

- `credits == 0` → buffer is **non-credited** (default; upstream behavior).
- `credits > 0` → buffer is **credited**; the pool starts full (`credits`
  available) and bounds *admitted-but-not-yet-departed* messages.

The credit pool maps onto a private `std::unique_ptr<CreditState> m_credit`,
which is `nullptr` exactly when `credits == 0`. `credits` is a **per-buffer**
(hence per-vnet) value because it is needed at construction to size the pool; the
topology sets it on each int-link buffer exactly as it already sets `buffer_size`
and `ordered`.

> **The return latency is not a buffer parameter.** `MessageBuffer` is a plain
> `SimObject`, not a `ClockedObject`, so it has no clock period to convert a
> cycle count into ticks. The credited link's return latency therefore lives on
> the **link**, as a new `credit_return_latency` (`Param.Cycles`) on
> **`SimpleIntLink`** ([`simple/SimpleLink.py`](src/mem/ruby/network/simple/SimpleLink.py)),
> next to the existing link `latency` and `buffers`. The driver/switch (a
> `ClockedObject`) reads it off the int link, converts it with `cyclesToTicks`,
> and passes the resulting tick delay to `returnCredit()` — exactly how
> `Throttle` already reads the link `latency`. `MessageBuffer` stores nothing
> about latency. See §3.2 and §9.
>
> It is **not** placed on `BasicLink`: that base class is shared with Garnet,
> which has its own credit flow control (`CreditLink`), so a credit param there
> would collide with an unrelated mechanism. `SimpleIntLink` scopes it to the
> SimpleNetwork path.

### Construction-time invariant (fatal on violation)

Validated in `CreditState`'s constructor; only checked for credited buffers:

- `buffer_size == 0 || buffer_size >= credits` — a credited buffer may be
  unbounded, but a bounded one must hold at least `credits` slots so an admitted
  message always has a landing slot.

### Sizing for throughput (bandwidth-delay product)

To sustain line rate (1 msg/cycle/channel) a credited link must size `credits`
to cover the **entire credit round-trip**, not just the forward latency. A credit
is tied up for the whole loop:

```
spend (enqueue, t0) --L_f forward--> consumer departs --returnCredit-->
    credit matures at ~ t0 + L_f + L_r
```

where `L_f` is the link forward latency (the `enqueue` `delta`) and `L_r` is
`credit_return_latency`. With only `C` credits, throughput is bounded by
`C / (L_f + L_r)`; to hit `channels` msgs/cycle the pool must be:

```
credits ≈ channels × (L_f + L_r + 1)
```

The `+1` covers the slot freeing only on the cycle after a dequeue (the same
reason the existing forward-only sizing uses `latency + 1`). If `credits` covers
only `L_f`, the producer stalls every time a message sits in the return-latency
part of the loop and the link never reaches line rate.

Because of the invariant above, **the buffer scales with the credits**. So the
throughput sizing in `SimpleIntLink.setup_buffers` generalizes from the
forward-only form to the full-loop form:

```python
# today (forward path only):
buffers[i].buffer_size = channels[i] * (self.latency + 1)

# credited (full credit loop):
buffers[i].credits     = channels[i] * (self.latency + self.credit_return_latency + 1)
buffers[i].buffer_size = buffers[i].credits          # satisfies buffer_size >= credits
buffers[i].max_dequeue_rate = channels[i]
```

**Caveat — staging adapter over-provisions the buffer.** When a consumer drains
matured messages into its own staging container (the OoO adapter, §9), `dequeue`
frees the buffer slot *before* `returnCredit` is called at real departure, so the
in-flight messages during the return window live in the staging container, not in
the `MessageBuffer`. The buffer's true peak occupancy is therefore below
`credits`, but the conservative `buffer_size >= credits` invariant still forces it
to be sized for the full pool. Sizing **credits** to `L_f + L_r` remains required
for throughput regardless; only the buffer's physical footprint is potentially
over-provisioned, and revisiting the invariant is the place to reclaim it.

---

## 3. Added methods

### 3.1 Credited-state query

```cpp
bool isCredited() const;   // does this buffer participate in credit flow control?
```

A `const`, side-effect-free predicate, exactly `m_credit != nullptr`. A consumer
uses it to branch on whether a downstream buffer is credited — e.g. to apply
credit-aware backpressure handling — rather than relying on `returnCredit()`
silently no-op'ing. Returns `false` on a non-credited buffer.

### 3.2 Credit-return API

```cpp
// Return `slots` credit(s) to the producer, becoming visible
// `credit_return_delay` ticks from `cur_time`. No-op on a non-credited buffer.
void returnCredit(Tick cur_time, Tick credit_return_delay,
                  unsigned slots = 1);
```

The signature deliberately mirrors `enqueue(msg, curTime, delta, …)`: a
`(current_time, tick_delta)` pair. Because the delay is supplied in **ticks** by
the caller, the buffer needs no clock and no `Cycles` conversion.

- This is the **only** path that returns credits to the pool. `dequeue()` never
  returns a credit (see §4.3).
- The consumer calls it when a message has *genuinely departed* downstream —
  which is decoupled from the `dequeue()` that moved the message out of the heap.
- Each call records `slots` against the maturity tick
  `cur_time + credit_return_delay`. Until that tick passes the slots are *pending*
  (not yet available); they rejoin the available pool when redeemed (§6).
- `credit_return_delay` must be **> 0** on a credited buffer: a 0-tick return
  would let a credit be reused in the tick it is freed, which is not a physical
  link, and the lazy-redemption path relies on a credit never maturing in the
  tick it is queued.
- Maturity ticks must be **non-decreasing** across successive calls (a fixed
  return latency driven by a monotonic clock guarantees this); violating it is a
  `gem5_assert` failure.
- `slots == 0` is a no-op.
- On a **non-credited** buffer (`m_credit == nullptr`) the whole call is a no-op,
  so a consumer can call it unconditionally without first testing whether the
  downstream buffer is credited.

### 3.3 Destructor

```cpp
~MessageBuffer() override;
```

Added out-of-line so `std::unique_ptr<CreditState>` can be held by a forward
declaration in the header (`CreditState` is defined only in the `.cc`). Defaulted
behavior; no scheduled events to tear down (redemption is lazy).

---

## 4. Modified methods

### 4.1 `areNSlotsAvailable(unsigned n, Tick curTime)` — credit-aware

On a credited buffer, **before** the existing slot/size check, it now returns
`false` when fewer than `n` credits are available:

```cpp
if (m_credit && !m_credit->hasCredit(n)) {
    return false;
}
// ... unchanged slot/size logic ...
```

(`CreditState::hasCredit` is an internal helper, not a public method.) Effect: a
producer that already gates admission on `areNSlotsAvailable()` automatically
respects credits with no code change. On a non-credited buffer the behavior is
byte-for-byte unchanged. This is the **only** producer-visible credit gate — there
is no separate public `hasCredit`/`availableCredits` query.

### 4.2 `enqueue(...)` — spends a credit

On a credited buffer, every `enqueue()` spends one credit up front:

```cpp
if (m_credit) {
    m_credit->spendCredit(delta);  // -1 credit, immediate
}
```

Contract:
- The producer is responsible for checking availability first via
  `areNSlotsAvailable()`. The buffer **does not block** — enqueuing without an
  available credit is a fatal `panic`.
- The forward latency `delta` must be non-zero on a credited buffer (fatal
  otherwise). Non-credited buffers keep the existing `allow_zero_latency` rule.

### 4.3 `dequeue(...)` — credit-neutral (semantics change)

`dequeue()`'s signature is unchanged, but its **meaning relative to credits** is
defined here: dequeuing removes the head message and accounts occupancy/stats,
but **does not touch credits**. Popping a message into the consumer's staging is
*not* the same event as the message departing downstream, so it does not free the
upstream slot. The credit is freed only by an explicit `returnCredit()`.

(Independently, a new private `canDequeue(Tick)` factors out the existing
`max_dequeue_rate` gate used by `isReady()`/`dequeue()`; this is not a credit
feature and does not change observable behavior.)

### 4.4 `clear()` — resets the credit pool

On a credited buffer, `clear()` now also resets the pool to full and drops all
pending returns:

```cpp
if (m_credit) {
    m_credit->clear();  // m_credits = maxCredits, returnQueue empty
}
```

No return event to deschedule (redemption is lazy).

### 4.5 Constructor — builds and validates credit state

`MessageBuffer(const Params&)` now constructs `m_credit` as a `CreditState` when
`p.credits > 0` (else leaves it `nullptr`), which enforces the §2 invariant.

---

## 5. Private additions (not public API; listed for completeness)

- `class CreditState` — owns the credit pool, the pending-return queue, and
  redemption. Defined in the `.cc`; forward-declared in the header.
- `std::unique_ptr<CreditState> m_credit` — `nullptr` ⇔ non-credited.
- `bool canDequeue(Tick current_time) const` — the `max_dequeue_rate` gate
  (refactor, not a credit feature).

> **No new stats in the first upstreamed version.** Credit counters (stalls,
> returns, occupancy, etc.) are deliberately left out to keep the initial change
> minimal; they can be added later without changing the public surface.

---

## 6. State

This section designs the **minimal internal state** for the credit mechanism. The
whole feature is contained in `CreditState`, reached through
`std::unique_ptr<CreditState> m_credit` (`nullptr` ⇔ disabled). The design targets
three properties:

1. **Nothing when disabled** — a non-credited buffer holds one null pointer and
   allocates no `CreditState` (§intro design goal).
2. **Allocate once, never again** — no per-operation heap traffic on the hot path.
3. **O(1) hot operations** — spend, redeem, and return are all constant time.

### 6.1 Fields — exactly three

The pending-return ring is `gem5::CircularQueue<Entry>`
([`base/circular_queue.hh`](src/base/circular_queue.hh)) — gem5's own
fixed-capacity ring (no need to hand-roll one, and gem5 does not use boost). It
backs itself with a `std::vector` sized once at construction and thereafter only
moves `size_t` head/size indices, so it does no heap traffic after construction;
it is already used on the O3 LSQ and prefetcher hot paths.

```cpp
// Defined in MessageBuffer.cc only (forward-declared in the header).
class MessageBuffer::CreditState
{
    struct Entry { Tick maturity; unsigned slots; };

    const unsigned        m_max_credits;  // pool size; reset target for clear()
    unsigned              m_available;     // credits free to spend right now
    CircularQueue<Entry>  m_pending;       // ring; capacity == m_max_credits
};
```

| Field | Role | Changes on |
| --- | --- | --- |
| `m_max_credits` | pool size; the value `clear()` resets to | never (const) |
| `m_available` | credits free to spend now — the value `areNSlotsAvailable` checks | spend (−1), redeem (+slots), clear (= max) |
| `m_pending` | returned-but-not-yet-matured credits, ordered by maturity | returnCredit (`push_back`), redeem (`pop_front`), clear (`flush`) |

`m_pending` is constructed with capacity `m_max_credits` (`CircularQueue<Entry>
m_pending{m_max_credits}`); its own `_capacity`/`_head`/`_size` replace the
hand-rolled index bookkeeping.

**Deliberately *not* stored, and why:**

- **No `spent`/outstanding counter** — implied by conservation
  `spent = m_max_credits − m_available − pending`; no operation needs it.
- **No running pending-sum** — redemption pops the head and adds straight to
  `m_available`; the minimal API reports no total-pending value.
- **No return-latency field** — the latency lives on `SimpleIntLink`; the buffer
  receives the delay as a `returnCredit` argument (§2, §3.2).
- **No stat members, no callback, no `EventFunctionWrapper`** — redemption is
  lazy (pulled on query), so there is no scheduled event to hold or tear down.
- **No `mutable`** — see §6.4.

### 6.2 The pending-return ring

A returned credit becomes spendable only at its **maturity tick**, so the state
must remember, per return, when it matures. Two structural facts make this cheap:

1. **Maturity ticks are monotonically non-decreasing.** A credited link returns
   with a fixed latency off a monotonic clock (the §3.2 assertion enforces it), so
   each new return matures at or after the previous one. Returns therefore form a
   **FIFO: push at the tail, redeem from the head.**
2. **At most `credits` entries can ever be live.** Conservation
   `available + spent + pending = m_max_credits` holds at all times, so
   `pending ≤ m_max_credits`; every entry carries ≥ 1 slot, hence
   `entries ≤ pending ≤ m_max_credits = credits`.

Fact 2 is the key optimization: the FIFO is a **fixed-capacity `CircularQueue` of
exactly `credits` entries**, sized once in the constructor. It never reallocates
and, under a correct consumer, never overflows — an overflow would mean returning
more credits than were spent. (A `std::deque` also works but allocates in chunks;
the ring is the zero-steady-state-allocation choice for a hot-path object, and
`CircularQueue` tracks its own size so capacity is exactly `credits`, not
`credits + 1`.)

> **Overflow must be guarded explicitly.** `CircularQueue::push_back` does *not*
> assert on a full queue — `advance_tail()` silently overwrites the oldest entry.
> So `returnCredit` must check `assert(!m_pending.full())` itself before pushing.
> Fact 2 guarantees it never legitimately fills, so this is a bug-catcher for an
> over-returning consumer, but it has to be written by us, not relied upon from
> the container.

Entries **coalesce by tick**: `returnCredit` inspects `m_pending.back()` and, if
its maturity equals the new maturity, just adds to that entry's `slots` instead of
pushing. A switch that grants several messages in one cycle — all maturing at the
same tick — thus produces **one** entry and **one** redeem, no matter how many
`returnCredit(…, slots = 1)` calls it makes.

### 6.3 Operations (all O(1) amortized)

- **redeem** (private; called at the top of `areNSlotsAvailable` and before
  `enqueue`'s spend): while `!m_pending.empty()` and `m_pending.front().maturity
  ≤ curTick()`, add `m_pending.front().slots` to `m_available` and
  `m_pending.pop_front()`.
- **spend** (`enqueue`): redeem, then `--m_available` (panic if it is already 0 —
  the caller must gate on `areNSlotsAvailable`).
- **returnCredit**: assert `credit_return_delay > 0` and `!m_pending.full()`; with
  `maturity = cur_time + credit_return_delay` (assert ≥ `m_pending.back().maturity`
  for monotonicity), either `m_pending.back().slots += slots` if the tail matches
  or `m_pending.push_back({maturity, slots})`.
- **clear**: `m_available = m_max_credits; m_pending.flush();`.

### 6.4 Const-ness — no `mutable` needed

Lazy redemption mutates `m_available` and the ring, but the minimal API has **no
const method that redeems**: `areNSlotsAvailable`, `enqueue`, `returnCredit`, and
`clear` are all non-const, and `isCredited()` only tests the pointer. So redemption
always runs in non-const context and the fields stay plain (non-`mutable`).
Re-introducing a const query getter (e.g. `availableCredits() const`) would force
`mutable` back — one more reason those getters are kept out (§9).

### 6.5 Footprint

- **Disabled (`credits == 0`):** one null `unique_ptr` (8 B) inside
  `MessageBuffer`; no `CreditState`, no ring, no allocation, no construction.
- **Enabled:** `sizeof(CreditState)` plus a single ring allocation of `credits`
  entries (each `{Tick, unsigned}` ≈ 16 B aligned), made once at construction and
  never resized.

---

## 7. Credit lifecycle (semantics the API guarantees)

```
        enqueue() / spendCredit()              dequeue() (in-order, head-only)
producer ----------------------------> buffer ----------------------------> consumer
   ^         (-1 credit, immediate)            |  message moves into the
   |                                           |  consumer's own staging
   |       returnCredit(t, delay)  <-----------+  consumer calls this on the
   |       (+1 after delay, lazy)                 message's real departure
   +-------------------------------------------  downstream
```

- **Spend** is immediate, at admission (`enqueue`).
- **Pop** (`dequeue`) is credit-neutral.
- **Return** (`returnCredit`) is explicit and delayed by the caller-supplied
  `credit_return_delay`.
- **Redemption is lazy / polled:** there is **no** producer notification and
  **no scheduled event** when a credit matures. Matured returns are pulled back
  into the available pool the next time admission is evaluated (inside the
  `areNSlotsAvailable` credit check and `enqueue`'s spend). A blocked producer
  learns of a freed credit by polling again on a later cycle. This is safe
  because `credit_return_delay` is non-zero (a credit never matures in the tick
  it is queued) and under-counting between maturity and the next poll can only
  delay a grant, never over-grant.

Because spend happens at admission and return only on real departure, the pool
bounds everything the consumer is still holding on the producer's behalf — which
is exactly what a credited link is meant to bound.

---

## 8. Behavior by configuration

| `credits` | Admission gate | Pop selection | Credit return |
| --- | --- | --- | --- |
| `0` | slot count only | strict-FIFO head-only `dequeue()` | n/a (`returnCredit` is a no-op) |
| `>0` | slots **and** credits | strict-FIFO head-only `dequeue()` | explicit `returnCredit()`, delayed by the caller's `credit_return_delay`, redeemed lazily |

> Out-of-order arbitration is **not** part of this API — the buffer is always
> strict-FIFO. It is a consumer policy and is documented separately.

---

## 9. Out of scope for this API section (tracked separately)

- **Return-latency configuration** — a new `credit_return_latency`
  (`Param.Cycles`) on **`SimpleIntLink`** (not `BasicLink`, to avoid colliding
  with Garnet's `CreditLink`). The driver/switch (a `ClockedObject`) reads it off
  the int link, `cyclesToTicks`-converts it, and passes the tick delay to
  `returnCredit()`. Not a `MessageBuffer` param. (`credits`, by contrast, *is* a
  `MessageBuffer` param — see §2 — since it is needed at construction.)
- **Inspection getters** — `hasCredit`, `availableCredits`, `maxCredits`,
  `creditReturnLatency`, `getMaxSize`. Useful for unit tests and a future OoO
  adapter, but not required by the credit mechanism itself, so omitted from the
  minimal surface. (`isCredited()` is kept — see §3.1.)
- **Stats** — the credit counter/stat group.
- **External links (`SimpleExtLink`, controller↔switch).** Credited mode is
  scoped to **int-link (router↔router) buffers** — the hop where link congestion
  is modeled — and `credit_return_latency` lives on `SimpleIntLink` accordingly.
  `SimpleExtLink` owns **no** `MessageBuffer`s (only `SimpleIntLink` declares
  `buffers`/`setup_buffers`; ext-link buffering is the controller's own buffers),
  so there is nothing on it to credit. Crediting endpoint injection/ejection
  would be a separate extension — it would require making the ext-link buffer a
  credited `MessageBuffer`, a consumer at each end to call `returnCredit()` (the
  ejection side is SLICC-generated, hence intrusive), and a latency home on
  `SimpleExtLink`. The conventional alternative — a credited edge/NIC input
  buffer at the switch — keeps it on the int-link/switch side anyway.
- **The consumer-side out-of-order adapter** (e.g. an XP switch) and its
  functional-access overrides; `getMaxSize()` would return with it to size the
  reorder window.
- **`TraceState`/`traceState()`** observation surface (buffer tracing/FST), an
  independent observability feature.
- **Polling-vs-callback backpressure** and any future credit-return callback.

---

## 10. Functional reads under credited timing (`--suppress-func-errors`)

Credited mode is **functionally transparent**: it changes message *timing*
(flow-control backpressure), not coherence. Credited in-flight buffers are the
int-link buffers already registered for functional traversal
(`SimpleNetwork::m_int_link_buffers`), so a functional read/write still reaches
every message a credited link is holding.

It does, however, **shift timing**, and that interacts badly with one pre-existing
Ruby limitation worth recording:

- A functional read of a line that is **mid-transaction** — no controller holds a
  readable copy (`num_ro == num_rw == 0`), some controller is `Busy`/`Maybe_Stale`,
  and the data message is not yet in any buffer (the request is still in flight,
  the data response not yet generated) — **fails** (`RubySystem::simpleFunctionalRead`
  returns `false` → `pkt->isError()`). Protocols can avoid this by giving
  `Maybe_Stale` controllers a `functionalReadPriority >= 0`; **MI_example does
  not**, so it relies on the data being findable in transit.
- Credited backpressure **widens the window** in which a line sits in that
  transient state, so a deterministic functional probe is more likely to land in
  it. This is a property of *any* timing change, not of credited accounting:
  sweeping `credit_return_latency` makes `ruby_mem_test` fail at **different random
  addresses for different latencies** (and pass for others) — the signature of a
  timing-exposed transient, not a corruption.

**This is not a credited-mode correctness bug.** Evidence: the same multicore run
with functional accesses disabled completes the full 20M ticks, and the normal
(non-functional) reads — which verify data — never fail; only the instantaneous
functional probe trips.

**Consequence for the tests.** The `ruby_mem_test-simple-extra` and
`-simple-extra-multicore` targets exercise credited mode with `--functional 10`
(so functional traversal of credited buffers is covered) plus
**`--suppress-func-errors`**. That flag is gem5's sanctioned tolerance for exactly
this situation (MemTest.py: *"the ability to suppress error responses on
functional accesses as Ruby needs this"*). It suppresses **only** the transient
functional-probe panic; normal reads still fully check data correctness, so
coherence remains under test. A protocol that implements `functionalReadPriority`
for `Maybe_Stale` would not need the flag.
