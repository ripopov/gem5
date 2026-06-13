# CreditedLinkBuffer - Requirements and gem5 Design

`CreditedLinkBuffer` is the link-level building block for making Ruby's
`SimpleNetwork` behave like a finite, credit-controlled NoC link while keeping
the network at message granularity. It models the receiver-side input storage
of one link/channel and the producer-visible credit count for that same
downstream storage.

The component is intentionally small: one producer, one consumer, one credit
pool, and one ordered admission stream of Ruby messages. Multi-channel or
multi-vnet links compose several buffers.

## 1. Problem statement

Ruby `SimpleNetwork` currently uses `MessageBuffer` as a powerful high-level
building block: it carries messages, models enqueue delay, exposes capacity,
drives wakeups, and participates in gem5's normal tracing and functional-access
machinery. That is the right style of abstraction for this work. The problem
is that the specific contract exposed by `MessageBuffer` is not specific enough
for a credited NoC link: it has capacity-based backpressure, but not delayed
credit return, an explicit credit lifetime, or oldest-eligible selection.

```text
Producer / throttle
    |
    | enqueue(delta = link latency)
    v
MessageBuffer at next switch input
    |
    | head-only dequeue by PerfectSwitch
    v
Next switch pipeline
```

In the current `SimpleNetwork` path the same queue already covers useful
high-level behavior:

- the forward delay line, through the `delta` passed to `enqueue()`;
- the input storage, through messages held in `MessageBuffer`;
- the backpressure signal, through `areNSlotsAvailable()`.

For credited NoC modelling, that contract leaves five gaps.

| Gap | Current behavior | Why it is a problem |
| --- | --- | --- |
| G1 | A slot freed by `dequeue()` is visible to the producer immediately. | There is no credit-return latency, so congestion propagates too quickly and credit-limited throughput cannot be reproduced. |
| G2 | The only backpressure knob is `buffer_size`. | Credit count cannot be sized independently from physical storage. |
| G3 | Link latency, storage depth, and flow-control timing are not independently configurable. | A slow distant link and a congested downstream buffer are hard to distinguish. |
| G4 | `PerfectSwitch` only examines the head of each input buffer. | A message for a blocked output can stall newer messages for free outputs. |
| G5 | The network lacks a reusable credited-buffer primitive. | New experiments would need ad hoc buffering, stats, wakeups, and checkpoint handling. |

`CreditedLinkBuffer` solves these gaps without lowering the network model to
individual wires and RTL queues. It remains one gem5-level component, but its
public contract includes the NoC-level semantics we need: a producer may inject
only when it owns a credit, sending consumes that credit, and the credit
returns only after the message leaves the downstream input buffer and the
configured return latency has elapsed.

The model should preserve the existing `SimpleNetwork` behavior when credited
mode is disabled. This keeps classic `simple` regressions and existing Ruby
controllers out of the blast radius.

## 2. List of features Credited Link Buffer should support

### 2.1 Explicit credit pool

Each credited buffer has:

- `max_credits`: the number of slots the producer is allowed to have in flight;
- `credits`: the currently available producer-side credits;
- `credit_return_latency`: the delay between freeing a downstream slot and
  making that credit visible to the producer.

`credits` starts at `max_credits`. A successful send decrements it. A delayed
credit-return event increments it.

### 2.2 Credit count decoupled from storage capacity

`max_credits` is a flow-control parameter, not a synonym for `buffer_size`.
The storage capacity must be at least as large as the credit pool:

```text
buffer_size >= max_credits
```

This makes the credit loop the intentional bottleneck. Extra storage may exist
for implementation slack, but it must not silently change credit-limited
throughput.

### 2.3 Explicit forward and return latency knobs

The forward link delay remains the `delta` used when the producer enqueues a
message. The credit-return delay is a separate public knob on the same
high-level component:

```text
send at T
  -> message becomes ready at T + forward_link_latency
  -> message waits until it wins downstream arbitration
  -> credit returns at departure + credit_return_latency
```

The minimum useful round-trip credit time is:

```text
RTT_min = forward_link_latency + 1 cycle of downstream residency
          + credit_return_latency
```

With one-slot messages, an uncongested link needs roughly `RTT_min` credits to
send one message per cycle. Fewer credits intentionally throttle the producer.

### 2.4 Credit return on departure, not on arrival

A credit represents a downstream input slot. The slot is occupied from
producer send until the message leaves the buffer for the next switch stage or
output link. Therefore:

- do not return a credit when the message arrives;
- do not return a credit when the message becomes ready;
- return the credit only when the consumer removes the message from the
  credited buffer.

This is the most important timing rule in the design.

### 2.5 Producer wakeup on credit return

When credits are exhausted, the producer must stall rather than dropping or
polling aggressively. A delayed credit return should wake the producer through
a callback or event hook. The producer can then retry the send path.

The wakeup path should collapse repeated credit returns in the same cycle into
one scheduled producer wakeup.

### 2.6 Ready-message selection for HoL elimination

The consumer must be able to select the oldest ready message satisfying a
predicate, not only the physical head:

```text
oldest ready message where route(message) == output
                         and output has space/credit
```

This supports head-of-line (HoL) elimination inside a link/vnet buffer: a
message for a free output can pass an older ready message whose output is
blocked. The selector must still preserve order among messages that target the
same output.

The same buffer should also support a head-only mode for behavior-equivalent
bring-up and for router models that intentionally do not eliminate HoL.

### 2.7 One buffer per independent flow-control domain

The component models one credit pool. A network link with multiple vnets or
physical channels should instantiate one `CreditedLinkBuffer` per independent
credit domain. A starved vnet must not consume credits from another vnet.

### 2.8 Functional access, checkpointing, and stats

The buffer must remain visible to gem5's normal infrastructure:

- functional reads and writes must see every message in the credited storage;
- serialization must checkpoint all queued messages and credit state;
- stats must expose credit stalls, credit occupancy, credit returns, storage
  occupancy, and HoL skips.

For the disabled mode, existing `MessageBuffer` stats and behavior should stay
unchanged.

### 2.9 Disabled mode

`max_credits == 0` means credited mode is disabled. In disabled mode:

- `hasCredit(n)` returns true;
- enqueue/dequeue timing follows the existing `MessageBuffer` behavior;
- no credit-return events are scheduled;
- classic `SimpleNetwork` behavior remains the reference.

## 3. Proposed public API

The API is split by role. Producer-side calls spend credits and insert
messages. Consumer-side calls select ready messages and return credits after
departure.

### 3.1 Parameters

Python-facing parameters should be available either on a
`CreditedLinkBuffer` SimObject or as guarded `MessageBuffer` parameters,
depending on the implementation option chosen in section 5.

```python
credits = Param.Unsigned(
    0,
    "Credit pool size; 0 disables credited mode",
)

credit_return_latency = Param.Cycles(
    1,
    "Cycles from downstream departure until credit is visible upstream",
)

enable_ooo_pop = Param.Bool(
    True,
    "Allow oldest-eligible selection instead of head-only dequeue",
)
```

The constructor should reject invalid credited configurations:

- `credits > 0` with `buffer_size != 0` and `buffer_size < credits`;
- credited enqueue with zero forward latency;
- `enable_ooo_pop == true` on a backend that cannot remove from the selected
  position.

### 3.2 Producer-side API

```cpp
bool isCredited() const;
bool hasCredit(unsigned slots = 1) const;
unsigned availableCredits() const;
unsigned maxCredits() const;

void registerCreditCallback(std::function<void()> callback);
void unregisterCreditCallback();

void enqueue(MsgPtr message,
             Tick cur_time,
             Tick forward_latency,
             bool ruby_is_random,
             bool ruby_warmup,
             bool bypass_strict_fifo = false);
```

Producer-side semantics:

| Call | Credited-mode behavior |
| --- | --- |
| `hasCredit(slots)` | True when at least `slots` credits are available. |
| `enqueue(...)` | Asserts `hasCredit(1)` and `forward_latency > 0`, decrements `credits`, and inserts the message with ready time `cur_time + forward_latency`. |
| `registerCreditCallback(cb)` | Registers the event hook used to wake a stalled producer after delayed credit return. |

If the class extends `MessageBuffer`, existing call sites can continue to use
the familiar `enqueue()` signature. The credited branch must live where the
actual enqueue mutation happens, not only in a subclass method that could be
bypassed through a base pointer.

### 3.3 Consumer-side API

```cpp
struct CreditedBufferHandle
{
    bool valid() const;
};

bool isReady(Tick cur_time) const;
Tick readyTime() const;

CreditedBufferHandle selectEligible(
    const std::function<bool(const Message&)>& predicate,
    Tick cur_time) const;

const MsgPtr& peekAt(CreditedBufferHandle handle) const;
Tick popAt(CreditedBufferHandle handle,
           Tick cur_time,
           unsigned slots = 1);
```

Consumer-side semantics:

| Call | Behavior |
| --- | --- |
| `isReady(cur_time)` | True when at least one message has reached its ready time. |
| `selectEligible(pred, cur_time)` | Returns the oldest ready message satisfying `pred`, or an invalid handle. |
| `peekAt(handle)` | Reads the selected message without removing it. |
| `popAt(handle, cur_time, slots)` | Removes the selected message and schedules a credit return for `slots`. |

Head-only compatibility can be expressed as a degenerate selector: select the
head only if it is ready and the downstream output is available.

### 3.4 Same-tick and event-ordering rules

The implementation must define one consistent rule for when returned credits
become visible:

- a credit-return event scheduled for tick `T` increments `credits` before the
  producer's send decision for tick `T`;
- a credit spent by a message cannot return before that message has departed;
- credited enqueue rejects zero forward latency, so a send cannot credit
  itself in the same tick.

`credit_return_latency == 0` may mean "visible on the departure edge" if the
event priority is documented. It must not mean classic instantaneous
`areNSlotsAvailable()` behavior. Classic behavior is the disabled mode.

### 3.5 Invariants

The implementation should assert these invariants close to the mutation that
could violate them:

- `0 <= credits <= max_credits`;
- one successful credited enqueue consumes exactly one credit by default;
- one `popAt()` schedules exactly one credit return by default;
- credit returns are never scheduled on message arrival or maturation;
- functional access and serialization include all storage containers used by
  the chosen backend.

## 4. Design principles

### 4.1 Keep a high-level component, expose NoC-level knobs

`CreditedLinkBuffer` should not decompose the link into a set of low-level
objects. It should stay a single instrumentable gem5 component that owns the
message storage, credit accounting, producer wakeup, and stats. What changes is
the public contract: forward latency, storage capacity, credit count, and
credit-return latency become explicit knobs so experiments can distinguish
distance, contention, and credit sizing.

### 4.2 Credits are producer-visible state with delayed updates

The producer decides whether it may send by reading `hasCredit()`. The count
may be hosted on the buffer object for gem5 convenience, but it must change as
if it were upstream-visible state: decrement at send, increment only through a
delayed credit-return event.

No producer should infer remote buffer occupancy directly from the downstream
container size.

### 4.3 Credit lifetime matches slot lifetime

A message owns a downstream input slot until the consumer removes it from the
credited buffer. The credit lifetime must match that slot lifetime. Returning
the credit earlier changes both throughput and backpressure timing.

### 4.4 Keep the default network path unchanged

Credited behavior is opt-in. Plain `MessageBuffer`, classic `SimpleNetwork`,
and SLICC controller queues should keep their existing behavior unless a
specific network class instantiates or enables credited buffers.

### 4.5 Compose per-vnet behavior instead of overloading one object

The buffer models one flow-control domain. Multiple CHI channels, vnets, or
physical lanes should be represented by multiple buffers. This keeps the credit
accounting local and makes starvation/debugging easier.

### 4.6 Preserve gem5 observability

The buffer should not become invisible to functional access, checkpointing,
statistics, or tracing. Any backend that adds a second container must update
all of those surfaces.

### 4.7 Make HoL elimination explicit

Oldest-eligible selection is a router feature, not a side effect of the storage
container. The API exposes it directly through `selectEligible()` and `popAt()`
so the XP arbiter can choose between head-only and HoL-eliminating behavior.

## 5. Implementation options

The semantics above are fixed. The storage backend is still a design choice.

### 5.1 Option A: extend MessageBuffer with credited mode

This option keeps `CreditedLinkBuffer` as a `MessageBuffer`-compatible object.
It adds guarded credited-mode state to the existing buffer implementation and
uses a Python subclass for configuration.

```cpp
class CreditedLinkBuffer : public MessageBuffer
{
  public:
    // Producer API: hasCredit(), availableCredits(), enqueue()
    // Consumer API: selectEligible(), peekAt(), popAt()

  private:
    bool m_creditEnabled;
    unsigned m_credits;
    unsigned m_maxCredits;
    Cycles m_creditReturnLatency;
    std::function<void()> m_creditCallback;
};
```

Two storage layouts are possible inside this option.

| Layout | Shape | Trade-off |
| --- | --- | --- |
| Heap plus ready deque | Keep the inherited priority heap for maturation, then move mature entries into a ready deque. | Reuses existing ready-time scheduling, but functional access, occupancy, and serialization must cover both containers. |
| Single scan container | Store entries with `ready_tick` and scan for ready/eligible messages. | Simpler and likely fast enough for small NoC buffers, but reuses less of the current heap behavior. |

Benefits:

- easiest fit for `SimpleIntLink.buffers`, which are already
  `VectorParam.MessageBuffer`;
- reuses existing `MessageBuffer` wakeup, randomization, tracing, stats base,
  and functional-access conventions;
- keeps endpoint controller queues on the familiar type.

Costs:

- existing `MessageBuffer` methods are not all virtual, so credited behavior
  must be implemented in the base mutation paths or all credited call sites
  must hold the concrete type;
- the class carries some controller-oriented features that the NoC link does
  not need;
- adding a ready deque requires careful updates to `getSize()`, `isEmpty()`,
  `functionalRead()`, `functionalWrite()`, and serialization.

This is the recommended first implementation if the goal is minimal disruption
to the existing `SimpleNetwork` wiring.

### 5.2 Option B: standalone SimObject

This option implements a focused `CreditedLinkBuffer` that does not inherit
from `MessageBuffer`.

```cpp
class CreditedLinkBuffer : public SimObject
{
  private:
    struct Entry
    {
        MsgPtr message;
        Tick readyTick;
    };

    std::deque<Entry> m_items;
    unsigned m_credits;
    unsigned m_maxCredits;
    Cycles m_creditReturnLatency;
};
```

Benefits:

- smaller and easier to reason about;
- no inherited stall-map, recycle, strict-FIFO, or controller queue behavior;
- arbitrary selection/removal can be designed directly around XP arbitration.

Costs:

- new network code must hold `CreditedLinkBuffer*` rather than
  `MessageBuffer*`;
- functional access, serialization, stats, debug printing, and wakeup handling
  must be implemented from scratch;
- it is less suitable for incremental replacement of `SimpleIntLink.buffers`.

This is attractive for a new XP-only network path where compatibility with
classic `Switch` and `Throttle` is not required.

### 5.3 Option C: credited wrapper around MessageBuffer

This option stores a plain `MessageBuffer` internally and adds credit state in
a wrapper object.

Benefits:

- avoids modifying `MessageBuffer` internals for the credit counter;
- keeps the normal queue as the storage backend.

Costs:

- arbitrary `popAt()` is difficult if the wrapped buffer only exposes head
  access;
- functional access and serialization must cross object boundaries;
- existing call sites using `MessageBuffer*` can bypass the wrapper unless the
  network is fully converted.

This option is viable only for head-only behavior or for a larger network
refactor. It is not the preferred path for HoL-eliminating XP arbitration.

### 5.4 Rejected shape: per-output demux lanes as the credit boundary

A tempting implementation is to demultiplex each input into one FIFO per output
and arbitrate among lane heads. That avoids arbitrary removal from a single
container, but it changes the credit boundary unless each lane is separately
credited. If the original input credit returns when a message moves into a
lane, the credit returns before downstream arbitration and the timing is wrong.

Use one credited input buffer with `selectEligible()` unless the hardware model
really has separate credited per-output input lanes.

## 6. Integration with SimpleNetwork / New Simple Network

### 6.1 Classic SimpleNetwork baseline

The relevant existing paths are:

- `PerfectSwitch` routes from input buffers into per-output `port_buffers`;
- `Throttle` drains `port_buffers` onto the next link;
- both stages use `areNSlotsAvailable()` as instantaneous downstream
  backpressure;
- internal-link buffers are created by `SimpleIntLink.setup_buffers()`;
- switch-local `port_buffers` are created by `Switch.setup_buffers()`.

`CreditedLinkBuffer` should not be enabled in this baseline path by default.
Classic `simple` remains the compatibility reference.

### 6.2 New Simple Network placement

For the XP-oriented network described in `NewSimpleNetwork.md`, the credited
buffer belongs at the receiver side of each internal link and vnet/channel:

```text
upstream XP link driver
    |
    | hasCredit() / enqueue(delta = t_link)
    v
CreditedLinkBuffer for one downstream input vnet/channel
    |
    | selectEligible(route/output predicate)
    v
XP arbiter and staging buffer
    |
    | popAt() schedules delayed credit return
    v
downstream output/link stage
```

The XP link driver spends the downstream buffer's credit when it sends. The XP
arbiter returns that credit when it grants the message out of the input buffer
into the next local stage.

### 6.3 Mapping to existing SimpleNetwork concepts

| Existing concept | XP / credited equivalent |
| --- | --- |
| `SimpleIntLink.buffers` | One `CreditedLinkBuffer` per vnet/channel for internal links. |
| `MessageBuffer::enqueue(delta)` | Still represents forward link latency. Credited mode also spends one credit. |
| `areNSlotsAvailable()` | Replaced by `hasCredit()` at link-boundary send decisions. |
| `PerfectSwitch` head peek/dequeue | Replaced by XP arbitration using `selectEligible()` and `popAt()`. |
| `SwitchPortBuffer` | Remains a local staging buffer; it may use ordinary same-cycle slot checks. |
| `Throttle` bandwidth accounting | Replaced or narrowed to the XP link driver, which enforces per-vnet/channel send rate and downstream credits. |

The distinction is important: credits model flow control across a link. Local
staging inside one XP can still use an immediate slot check because it is not a
distance-crossing credit loop.

### 6.4 Bring-up sequence

1. Add the buffer class and unit tests with `credits == 0` compatibility.
2. Instantiate credited internal-link buffers in the XP network only.
3. Teach the XP link driver to gate sends with `hasCredit()` and wake on credit
   return.
4. Keep XP arbitration head-only at first, using `popAt(head)` only.
5. Enable `selectEligible()` for HoL-eliminating arbitration.
6. Add per-vnet/channel credit and return-latency parameters to the NoC config.

This sequence keeps each behavior change measurable.

### 6.5 Validation expectations

The implementation is correct only if these tests pass:

- disabled mode produces the same behavior as plain `MessageBuffer`;
- credit conservation holds during execution:
  `available credits + occupied slots + pending credit returns == max_credits`;
  at drain, `available credits == max_credits`;
- an uncongested one-hop test shows the throughput knee at the configured
  credit round-trip time;
- delayed credit return makes backpressure propagate upstream hop by hop;
- a blocked-output HoL test shows that `selectEligible()` can drain messages
  for a free output while head-only mode cannot;
- checkpoint/restore and functional access see both maturing and ready
  messages.

### 6.6 Open implementation decisions

- Choose Option A or Option B before coding the XP network integration.
- Define the exact event priority for credit-return visibility.
- Decide whether endpoint links remain uncredited initially, as proposed in
  `NewSimpleNetwork.md`, or whether controller-facing queues also need
  credited mode.
- Decide whether credits always count one Ruby message or whether variable
  message sizes should consume multiple slots in non-CHI experiments.
