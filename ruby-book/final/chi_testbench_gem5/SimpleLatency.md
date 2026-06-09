# SimpleNetwork Latency And Buffering

This note describes the SimpleNetwork model used by the CHI testbench. It is
not a transaction-latency report; its purpose is to show which buffers,
latencies, and pipeline stages exist when the 4x4 mesh runs with:

```text
--network=simple
--simple-physical-channels=true    # default in the testbench driver
configs/example/noc_config/rbook_4x4.py
```

The testbench tiles connect directly to mesh routers with one ExtLink. They do
not instantiate the extra CHI_RNF side router.

## Every latency parameter

These are the values that matter for SimpleNetwork timing in this testbench.

```text
4x4 mesh routers

   0 --- 1 === 2 --- 3
   |     |     |     |
   4 --- 5 === 6 --- 7
  ===   ===   ===   ===
   8 --- 9 ===10 ---11
   |     |     |     |
  12 ---13 ===14 ---15

--- normal mesh link: router_link_latency = 2 cycles
=== cluster-boundary link: cross_link_latency = 7 cycles
```

`cross_link_latency` replaces `router_link_latency` for the directed links
listed in `cross_links`. It is not added on top of `router_link_latency`.

| Parameter | Value | Where it is used |
| --- | ---: | --- |
| `router_latency` | 4 cy | Config knob; here it feeds `int_routing_latency` and, +2, `ext_routing_latency`. SimpleNetwork has no separate switch-latency stage. |
| `int_routing_latency` | 4 cy | `PerfectSwitch` delay when routing to an internal router-to-router link. |
| `ext_routing_latency` | 6 cy | `PerfectSwitch` delay when routing to a controller endpoint. |
| `router_link_latency` | 2 cy | Normal directed mesh link delay, router to adjacent router. |
| `cross_link_latency` | 7 cy | Directed cluster-boundary mesh link delay. |
| `node_link_latency` | 1 cy | Directed external link delay from router to controller endpoint. |
| `node_router_latency` | 2 cy | Not on the tile path here; tiles are direct ExtLink nodes, not CHI_RNF side-router nodes. |
| `router_buffer_size` | 8 msg | Becomes `SimpleNetwork.buffer_size`; sizes each switch output/intermediate port buffer. |
| `link_bandwidth_factor` | 40 B/cy | Per-channel byte bandwidth used by each `Throttle`. |
| `number_of_virtual_networks` | 4 | CHI request, snoop, response, and data vnets. |
| `simple_physical_channels` | true | Sets `physical_vnets_channels = [1, 1, 1, 1]`. |
| `control_msg_size` | 8 B | SimpleNetwork wire size for control messages. |
| `data_msg_size` | 32 B | CHI data payload size; with header, one data beat is 40 B on the Simple link. |

Derived structural costs for one hop:

| Hop | Cost | Meaning |
| --- | ---: | --- |
| Normal internal hop | `4 + 2 = 6 cy` | Source switch routing plus normal mesh link. |
| Cross internal hop | `4 + 7 = 11 cy` | Source switch routing plus cluster-boundary mesh link. |
| Router to endpoint | `6 + 1 = 7 cy` | Source switch routing plus external link to controller. |
| Endpoint to local router | protocol enqueue latency | Controller output buffers are directly used as switch input queues. |

Protocol/controller enqueue latencies are separate from SimpleNetwork, but are
usually the first delay before a message becomes visible to a switch:

| CHI controller parameter | Value | Typical use |
| --- | ---: | --- |
| `request_latency` | 1 cy | Request message enqueue, such as `ReadUnique`. |
| `response_latency` | 1 cy | Response message enqueue. |
| `data_latency` | 1 cy | Data message enqueue; also spaces data beats from a controller. |
| `snoop_latency` | 1 cy | Snoop message enqueue. |
| `mandatory_queue_latency` | 1 cy | Sequencer request to controller mandatory queue. |
| `recycle_latency` | 10 cy | Protocol resource-stall retry delay, not a network link delay. |

The important SimpleNetwork buffers are:

| Buffer | Depth in this testbench | Backpressure check |
| --- | ---: | --- |
| Controller `MessageBuffer`s | usually infinite (`0`) | Protocol-specific endpoint queues. With `--allow-retryack=0`, HNF `reqIn` is forced to 2. |
| `Switch.port_buffers` | 8 msg per output port per vnet | Checked by `PerfectSwitch` before it leaves the input buffer. |
| Normal `SimpleIntLink.m_buffers` | `2 + 1 = 3` msg per vnet | Checked by `Throttle` before launching onto a normal mesh link. |
| Cross `SimpleIntLink.m_buffers` | `7 + 1 = 8` msg per vnet | Checked by `Throttle` before launching onto a cluster-boundary link. |
| `SimpleExtLink` output target | endpoint queue | Checked by `Throttle` before delivery to the controller. |

For internal links, the `SimpleIntLink.m_buffers` depth comes from
`src/mem/ruby/network/simple/SimpleLink.py` when physical channels are enabled:

```python
buffer_size = physical_vnets_channels[vnet] * (link_latency + 1)
max_dequeue_rate = physical_vnets_channels[vnet]
```

With one physical channel per vnet, that is simply:

```text
SimpleIntLink depth = link_latency + 1
SimpleIntLink max dequeue rate = 1 message/cycle
```

This is why increasing `cross_link_latency` also increases the cross-link input
buffer depth in SimpleNetwork. It does not increase the fixed 8-entry switch
port buffer, and it does not increase link bandwidth.

## How SimpleNetwork uses configured link latencies

The CHI testbench starts with the `NoC_Params` values in
`configs/example/noc_config/rbook_4x4.py`, falling back to
`configs/ruby/CHI_config.py` defaults for parameters not overridden there. The
`CustomMesh` topology then turns those values into per-link `latency` fields:

- `router_link_latency` is used for normal directed `SimpleIntLink`s.
- `cross_link_latency` replaces `router_link_latency` for directed links listed
  in `cross_links`.
- `node_link_latency` is used for each controller `SimpleExtLink`.

In C++, all three arrive as `BasicLink::m_latency`. `SimpleNetwork` passes that
latency to `Switch::addOutPort()`, which creates a `Throttle` for the directed
output. The latency is applied when the `Throttle` enqueues the message into
the downstream `MessageBuffer`:

```text
source switch output buffer -> Throttle -> downstream input buffer
                                      enqueue delay = link latency
```

Internal router-to-router links use that downstream buffer as the destination
router's input queue, so the link latency delays when the next router can route
the message. External controller links are asymmetric in SimpleNetwork:
`node_link_latency` is applied only on router-to-controller delivery. The
controller-to-router direction directly registers the controller output queue as
the switch input queue, so it has controller/protocol enqueue latency but no
separate `node_link_latency` stage.

## How a single message hop works in SimpleNetwork

![Generic SimpleNetwork architecture diagram showing one detailed hop from Router A to Router B. Router A is a SimpleNetwork Switch (BasicRouter) holding one PerfectSwitch plus one Throttle and one port buffer per output-port-times-vnet. On the left, a controller (network endpoint) connects by a bidirectional ExtLink. Every MessageBuffer uses the same label style: the word MessageBuffer, then a direction line (Owner Input/Output arrow Peer), then the variable name with scope, then a FIFO slot row. Controller to switch is a single shared MessageBuffer labelled Controller Output arrow Network with variable Controller::m_toNetQueues[vnets]; this controller output buffer is registered directly as the switch input queue, so there is no router-side input buffer and no link latency on this direction, and the PerfectSwitch dequeues it directly. Switch to controller has two buffers separated by a Throttle: a switch output port buffer (A Router Output arrow Controller, Switch::port_buffers[directions x vnets]) drains through a Throttle (ExtLink out, node_link_latency) into the controller input MessageBuffer (Controller Input arrow Network, Controller::m_fromNetQueues[vnets], unbounded). Buffers are drawn as rectangles with filled/empty FIFO slots; bounded router buffers have a fixed number of slots, while unbounded controller endpoint queues trail off through a dashed open slot to an infinity symbol (no fixed capacity, no backpressure). Inside Router A, the buffered input is A Router Input arrow Other Router (SimpleIntLink::m_buffers[vnets], also the delayed link buffer); all inputs feed the PerfectSwitch, drawn as an orange cloud labelled routing logic, no storage: for each ready input msg it routes, checks areNSlotsAvailable(1) on the output buffer (backpressure), and enqueues into it with no crossbar bandwidth limit, so many msgs per cycle can enter one output buffer bounded only by its free slots. The PerfectSwitch writes into per-output-port buffers (Switch::port_buffers[directions x vnets]) and becomes ready only after the routing latency (int_routing_latency for internal links, ext_routing_latency for endpoint links). Router A shows an output port buffer A Router Output arrow B Router and another A Router Output arrow Other Router. Each output port buffer drains through its own Throttle, drawn as a red cloud (byte budget = link_bandwidth_factor, admits while budget and slots remain). The Throttle A to B enqueues into Router B, and the message becomes ready only after the link_latency. Router B shows only the single B Router Input arrow A Router (SimpleIntLink::m_buffers[vnets], the delayed A to B link) feeding PerfectSwitch B. A legend explains bounded MessageBuffer (router) = fixed N slots enforcing backpressure via areNSlotsAvailable(), unbounded MessageBuffer (controller endpoint) = slots trailing off to infinity with no backpressure, PerfectSwitch = routing logic with no storage applying routing latency on enqueue to the output buffer, and Throttle = per-output bandwidth plus link-latency gate applying link latency on enqueue to the downstream buffer.](resources/simple_network_hop.svg)

SimpleNetwork models each directed switch output as two buffers separated by a
`Throttle`. The first buffer belongs to the source switch output port. The
second buffer belongs to the directed link, but it is also the destination
switch's input queue.

```text
Legend:
  ╔═════╗  MessageBuffer / FIFO storage; consumes capacity and backpressure
  ┌─────┐  logic stage; no packet storage modeled here

One directed internal link, one vnet: A → B

╭── Router A / SimpleNetwork Switch A ───────────────────────────────────────╮
│ ╔══════════════════════════════════════════════════════════════════════╗   │
│ ║ BUFFER: A input queue                                                ║   │
│ ║ controller output buffer or previous SimpleIntLink.m_buffers         ║   │
│ ╚══════════════════════════════════════════════════════════════════════╝   │
│                                   │ ready message                          │
│                                   ▼                                        │
│ ┌──────────────────────────────────────────────────────────────────────┐   │
│ │ LOGIC: PerfectSwitch::operateMessageBuffer                           │   │
│ │ route, pick output; require one A output-buffer slot                 │   │
│ │ enqueue delay = int_routing_latency = 4 cycles                       │   │
│ └──────────────────────────────────────────────────────────────────────┘   │
│                                   ▼                                        │
│ ╔══════════════════════════════════════════════════════════════════════╗   │
│ ║ BUFFER: A output port FIFO                                           ║   │
│ ║ Switch.port_buffers[vnet]                                            ║   │
│ ║ capacity = router_buffer_size = 8 messages                           ║   │
│ ╚══════════════════════════════════════════════════════════════════════╝   │
│                                   ▼                                        │
│ ┌──────────────────────────────────────────────────────────────────────┐   │
│ │ LOGIC: Throttle A→B                                                  │   │
│ │ require one free B link/input-buffer slot                            │   │
│ │ byte budget = 40 B/cycle; launch while budget and slots remain       │   │
│ └──────────────────────────────────────────────────────────────────────┘   │
╰────────────────────────────────────────────────────────────────────────────╯
                                    │ enqueue delay = link latency L
                                    ▼
╭── Directed Link A→B / Router B input queue ────────────────────────────────╮
│ ╔══════════════════════════════════════════════════════════════════════╗   │
│ ║ BUFFER: B link/input FIFO                                            ║   │
│ ║ SimpleIntLink.m_buffers[vnet]                                        ║   │
│ ║ capacity = L + 1 messages                                            ║   │
│ ║ max dequeue = 1 msg/cycle; ready after L cycles                      ║   │
│ ╚══════════════════════════════════════════════════════════════════════╝   │
╰────────────────────────────────────────────────────────────────────────────╯
                                    ▼
╭── Router B / SimpleNetwork Switch B ───────────────────────────────────────╮
│ ┌──────────────────────────────────────────────────────────────────────┐   │
│ │ LOGIC: PerfectSwitch::operateMessageBuffer                           │   │
│ │ consume ready message from B link/input FIFO                         │   │
│ │ route to local endpoint or B next output port                        │   │
│ └──────────────────────────────────────────────────────────────────────┘   │
│                                   ▼                                        │
│ ╔══════════════════════════════════════════════════════════════════════╗   │
│ ║ BUFFER: B output port FIFO                                           ║   │
│ ║ Switch.port_buffers[vnet]                                            ║   │
│ ║ capacity = router_buffer_size = 8 messages                           ║   │
│ ╚══════════════════════════════════════════════════════════════════════╝   │
╰────────────────────────────────────────────────────────────────────────────╯
```

For a normal A to B mesh link:

```text
L = router_link_latency = 2
A output buffer depth = 8
B SimpleIntLink buffer depth = 3
structural hop latency = int_routing_latency + L = 4 + 2 = 6 cycles
```

For a cross-cluster A to B mesh link:

```text
L = cross_link_latency = 7
A output buffer depth = 8
B SimpleIntLink buffer depth = 8
structural hop latency = int_routing_latency + L = 4 + 7 = 11 cycles
```

The link buffer occupies a slot immediately when `Throttle` enqueues the
message, even though the message is not ready to be consumed until `L` cycles
later. Those delayed messages are the SimpleNetwork approximation of packets in
flight on the wire.

The source switch stage is:

```text
PerfectSwitch input -> A Switch.port_buffer
```

`PerfectSwitch` checks `areNSlotsAvailable(1)` on the selected output port
buffer. If there is no slot, the message stays in the input buffer and the
switch retries later. This is upstream backpressure.

### The switch has no per-cycle crossbar limit

The `PerfectSwitch` is "perfect": it models no internal crossbar or output
write-port bandwidth. In one `wakeup()` (a single clock edge) it can move many
messages into the same output buffer:

- `operateMessageBuffer()` drains a single input with a
  `while (buffer->isReady(current_time))` loop, so multiple ready messages from
  the *same* input are popped in one wakeup.
- `operateVnet()` iterates over *all* input ports for the vnet, so messages
  from *several* inputs can be enqueued into the same output buffer in the same
  cycle.

The only gate is `areNSlotsAvailable(1)` on the target output buffer. Writes
into one output buffer per cycle are therefore bounded solely by that buffer's
free capacity (`router_buffer_size = 8`), not by any switch port. When the
output buffer fills, the routing check returns `enough = false`, the switch
`break`s out of that input and reschedules `+1` cycle.

The real shaping is downstream at the `Throttle` draining the output port
buffer onto the link: it is byte-budget limited, and it also requires a free
downstream slot.

The link launch stage is:

```text
A Switch.port_buffer -> Throttle -> B SimpleIntLink.m_buffers
```

`Throttle` checks `areNSlotsAvailable(1)` on the downstream link/input buffer.
If there is no slot, it stops launching. That lets A's output port buffer fill,
which then blocks A's `PerfectSwitch`.

The destination switch stage is:

```text
B SimpleIntLink.m_buffers -> B PerfectSwitch -> B output port buffer
```

If B cannot forward because the next output is blocked, the message remains in
`B SimpleIntLink.m_buffers`. That buffer is both the delayed link queue and the
input queue to B's `PerfectSwitch`.

For one directed A to B link, one vnet, and B blocked downstream, the local
A-to-B storage is:

```text
normal link, L = 2:

  A output port buffer                 B link/input buffer
  ╔═══╦═══╦═══╦═══╦═══╦═══╦═══╦═══╗    ╔═══╦═══╦═══╗
  ║ 1 ║ 2 ║ 3 ║ 4 ║ 5 ║ 6 ║ 7 ║ 8 ║ ─▶ ║ 1 ║ 2 ║ 3 ║
  ╚═══╩═══╩═══╩═══╩═══╩═══╩═══╩═══╝    ╚═══╩═══╩═══╝
       waiting before throttle           launched / delayed at B input

  total local A->B storage = 8 + 3 = 11 messages

cross link, L = 7:

  A output port buffer                 B link/input buffer
  ╔═══╦═══╦═══╦═══╦═══╦═══╦═══╦═══╗    ╔═══╦═══╦═══╦═══╦═══╦═══╦═══╦═══╗
  ║ 1 ║ 2 ║ 3 ║ 4 ║ 5 ║ 6 ║ 7 ║ 8 ║ ─▶ ║ 1 ║ 2 ║ 3 ║ 4 ║ 5 ║ 6 ║ 7 ║ 8 ║
  ╚═══╩═══╩═══╩═══╩═══╩═══╩═══╩═══╝    ╚═══╩═══╩═══╩═══╩═══╩═══╩═══╩═══╝
       waiting before throttle           launched / delayed at B input

  total local A->B storage = 8 + 8 = 16 messages
```

That count excludes buffers before A, buffers after B, and any other vnet. If
you include B's blocked output toward the next hop, add B's output port buffer
and the next link/input buffer, but that is no longer just the A-to-B segment.

This is close to credit-style flow control in the sense that a sender only
advances when the next queue has space. It is not a cycle-accurate credit-link
model: there are no explicit credit flits and no independent credit return
latency. Backpressure is represented by `areNSlotsAvailable()` checks on the
next `MessageBuffer`.

Increasing `cross_link_latency` therefore has two effects in this SimpleNetwork
configuration:

```text
1. It increases delay: structural cross-hop cost is 4 + cross_link_latency.
2. It increases the link/input buffer depth: cross_link_latency + 1.
```

It does not change the 8-entry source switch port buffer and it does not change
the throttle byte budget. If the destination drains more slowly than the source
offers traffic, the queues eventually fill and sustainable throughput falls to
the downstream drain rate. Extra latency only increases how many messages can be
resident in the delayed link buffer before that backpressure reaches the source.

## Throttle bandwidth: how `link_bandwidth_factor` works

Latency decides *when* a message becomes visible at the next buffer.
`link_bandwidth_factor` sets the `Throttle` byte budget used to admit messages
onto a directed output link. The two are independent: raising the factor never
reduces hop latency, and raising latency never changes the throttle budget.

### The byte budget

Every cycle the `Throttle` for a directed output is given a fresh byte budget
(`src/mem/ruby/network/simple/Throttle.cc`):

```text
per-cycle budget (bytes) = link_bandwidth_factor
cost of a message (bytes) = its wire size
```

Internally, `Throttle` compares `endpoint_bandwidth * link_bandwidth_factor`
against `message_size_bytes * 1000`; the default `endpoint_bandwidth = 1000`
makes `link_bandwidth_factor` equal to bytes per cycle, while other values
rescale all SimpleNetwork link budgets globally. When physical channels are
enabled, each vnet channel carries its own budget, so vnets do not share
bandwidth in the `Throttle`.

### The launch loop

For each vnet/channel the `Throttle` repeats, while budget remains and the
downstream buffer has a free slot:

```text
1. If not mid-message, take the head message, record its byte cost, and
   immediately move it: dequeue from the output port buffer and enqueue into
   the downstream link buffer with delay = link_latency.
2. spent = min(remaining_cost, remaining_budget)
   subtract spent from both the message's remaining cost and the budget.
```

Three consequences follow from the loop structure:

- A whole message is launched the instant it reaches the head, *as long as any
  budget is left* — the budget gate is checked before the message cost, not
  after. The message itself leaves in that cycle (subject only to
  `link_latency`); the byte accounting only governs when the *next* message may
  be admitted.
- A message's unpaid cost (`units_remaining`) persists across cycles. While it
  is non-zero the loop refuses to fetch a new message, so the link stays busy
  paying off the debt.
- The downstream buffer can still be the limiting factor. The throttle can only
  admit a message if `out->areNSlotsAvailable(1)` is true; the downstream link
  buffer's own dequeue rate may therefore cap sustained throughput below the
  byte budget.

### One example: two 15 B messages on a 20 B/cycle link

Assume one vnet/channel, a per-cycle byte budget of 20 B, two ready messages of
15 B each, and at least two free downstream slots:

```text
cycle N:
  budget = 20 B

  msg0 is admitted and forwarded immediately
    spent = min(15, 20) = 15 B
    budget = 5 B
    debt = 0

  budget is still > 0, so msg1 is also admitted and forwarded immediately
    spent = min(15, 5) = 5 B
    budget = 0
    debt = 10 B

cycle N+1:
  budget = 20 B

  msg1 debt is paid before any new message can be fetched
    spent = min(10, 20) = 10 B
    budget = 10 B
    debt = 0

  if another message is ready and the downstream buffer has a slot, it can now
  be admitted in this same cycle using the remaining 10 B budget
```

This example captures the important behavior:

- The throttle is byte-budgeted, not message-count limited.
- A message is admitted whenever any budget remains, even if the whole message
  does not fit in the remaining budget.
- The admitted message is forwarded immediately with `link_latency`; the
  unpaid byte cost becomes `units_remaining` debt.
- Debt is paid before the next message on that vnet/channel can be fetched.

If the throttle has continuous backlog and the downstream buffer never blocks,
this debt mechanism makes the long-run admission rate converge to the configured
byte budget. In the full SimpleNetwork path, sustained throughput can still be
lower if the downstream buffer's slots or dequeue rate become the bottleneck.
