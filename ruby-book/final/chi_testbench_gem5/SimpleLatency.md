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

## How a single message hop works in SimpleNetwork

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
│ │ bandwidth = 40 B/cycle; launch = 1 msg/cycle per vnet                │   │
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
the 1-message/cycle per-vnet launch rate. If the destination drains more slowly
than the source offers traffic, the queues eventually fill and sustainable
throughput falls to the downstream drain rate. Extra latency only increases how
many messages can be resident in the delayed link buffer before that
backpressure reaches the source.
