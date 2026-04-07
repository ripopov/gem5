# Chapter 9: Ruby Networks Before Garnet

> *Not every Ruby network model is cycle-accurate, and that distinction matters.*

You have traced Ruby requests from CPU ports through Sequencers, MessageBuffers, and controllers.
You understand how a load miss becomes a `GetS` message, how the directory responds, and how data flows back.
But we have waved our hands at one critical piece: the network itself.

This chapter opens the network black box.
Ruby actually has two distinct network implementations: `SimpleNetwork` (message-level transport) and `Garnet` (flit-level, cycle-accurate router modeling).
Most Ruby simulations use `SimpleNetwork` because it is faster and sufficient for many coherence studies.
But using it correctly requires understanding what it models and what it ignores.

By the end of this chapter you will understand:
- How `SimpleNetwork` moves messages from controller to controller
- What latency components `SimpleNetwork` actually accounts for
- The difference between message-level and flit-level transport
- When `SimpleNetwork` is sufficient and when you need Garnet
- How to configure and run SimpleNetwork-backed Ruby systems

---

### Table of Contents

- [9.1 The Network Abstraction Problem](#91-the-network-abstraction-problem)
  - [The Cost of Abstraction](#the-cost-of-abstraction)
  - [Intuition: Two Levels of Fidelity](#intuition-two-levels-of-fidelity)
- [9.2 SimpleNetwork Architecture](#92-simplenetwork-architecture)
  - [From MessageBuffer to MessageBuffer](#from-messagebuffer-to-messagebuffer)
  - [The Switch: PerfectSwitch + Intermediate Buffers + Throttle](#the-switch-perfectswitch--intermediate-buffers--throttle)
  - [Routing and Starvation Prevention](#routing-and-starvation-prevention)
  - [Working Model: How a Message Travels](#working-model-how-a-message-travels)
- [9.3 Latency Accounting in SimpleNetwork](#93-latency-accounting-in-simplenetwork)
  - [What SimpleNetwork Models](#what-simplenetwork-models)
  - [What SimpleNetwork Ignores](#what-simplenetwork-ignores)
  - [Formal: Latency Equation](#formal-latency-equation)
- [9.4 Topologies and Configuration](#94-topologies-and-configuration)
  - [Point-to-Point (Pt2Pt)](#point-to-point-pt2pt)
  - [Crossbar](#crossbar)
  - [Python Configuration](#python-configuration)
- [9.5 Running SimpleNetwork Experiments](#95-running-simplenetwork-experiments)
  - [Legacy Path with MI_example](#legacy-path-with-mi_example)
  - [Stdlib Path with SimplePt2Pt](#stdlib-path-with-simplept2pt)
- [9.6 Failure Modes: When Abstraction Misleads](#96-failure-modes-when-abstraction-misleads)
  - [Conflating Routing Latency with Link Latency](#conflating-routing-latency-with-link-latency)
  - [Underestimating Contention Effects](#underestimating-contention-effects)
  - [Protocol vs. Network Attribution](#protocol-vs-network-attribution)
- [9.7 How We Know This](#97-how-we-know-this)
- [Key Ideas](#key-ideas)
- [1-Page Mental Model](#1-page-mental-model)
- [Common Misconceptions](#common-misconceptions)
- [If You Remember One Thing](#if-you-remember-one-thing)
- [Exercises](#exercises)

---

## 9.1 The Network Abstraction Problem

Imagine you are modeling a directory protocol.
You care about coherence state transitions, writeback behavior, and directory entry allocation.
You set your link latency to 5 cycles and run experiments.

What does that 5-cycle latency actually represent?
In a real system, packets traverse:
1. Network interface (NI) injection
2. Route computation in the router
3. Virtual channel (VC) allocation
4. Switch allocation
5. Crossbar traversal
6. Link traversal
7. Credit return for flow control

Each of these takes cycles.
The sum depends on contention, buffer depth, and packet size.

`SimpleNetwork` collapses all of this into: a per-switch routing latency, a per-link propagation latency, and a per-output-port bandwidth check.
This is a deliberate tradeoff.

### The Cost of Abstraction

Consider two research scenarios:

**Scenario A:** You are evaluating a new directory protocol that reduces writeback frequency.
Network traffic drops 20%.
Does it matter exactly how many cycles each router takes?
Probably not—the protocol-level traffic reduction dominates.

**Scenario B:** You are proposing a new adaptive routing algorithm to reduce hotspot congestion.
This requires modeling how packets compete for VCs, how credit backpressure propagates, and how route choices affect latency under load.
SimpleNetwork cannot help—you need cycle-accurate router models.

The wrong choice wastes effort:
- Using Garnet for Scenario A slows simulation 3-5x for no benefit
- Using SimpleNetwork for Scenario B produces misleading results

### Intuition: Two Levels of Fidelity

```
┌─────────────────────────────────────────────────────────────────┐
│  SimpleNetwork (Message-Level)                                  │
│  ─────────────────────────────                                  │
│  • Messages move as whole units (no flits)                      │
│  • Per-hop latency = routing_latency + link_latency             │
│  • Bandwidth enforced at output throttles (coarse-grained)      │
│  • Output-port blocking when buffers are full                   │
│  • No VCs, no credit flow control, no switch arbitration        │
│                                                                 │
│  Use when: protocol studies, traffic reduction, functional      │
│  validation, fast simulation                                    │
├─────────────────────────────────────────────────────────────────┤
│  Garnet (Flit-Level, Cycle-Accurate)                            │
│  ───────────────────────────────────                            │
│  • Packets split into flits                                     │
│  • Router pipeline: RC → VA → SA → ST → LT                     │
│  • Credit-based flow control with backpressure                  │
│  • Contention at every pipeline stage                           │
│                                                                 │
│  Use when: NoC architecture, routing, saturation studies        │
└─────────────────────────────────────────────────────────────────┘
```

The key insight: **SimpleNetwork models bandwidth and latency at message granularity, not cycle-accurate contention dynamics within routers.**

---

## 9.2 SimpleNetwork Architecture

`SimpleNetwork` (`src/mem/ruby/network/simple/SimpleNetwork.hh`) inherits from the `Network` base class (itself a `ClockedObject`).
It provides message transport between Ruby controllers using configurable latency and bandwidth, without modeling flit-level router pipelines.

### From MessageBuffer to MessageBuffer

In Chapter 6, you learned that controllers communicate via `MessageBuffer` objects.
Each controller has:
- `in_port` buffers: Messages arrive here from the network
- `out_port` buffers: Messages depart here into the network

The network sits between these buffers:

```
Controller A                    Network                     Controller B
┌─────────────┐            ┌──────────────┐              ┌─────────────┐
│ out_port[0] │───────────▶│   Switch 0   │─────────────▶│ in_port[0]  │
│  (vnet 0)   │   Link     │  + Throttle  │     Link     │  (vnet 0)   │
└─────────────┘            └──────────────┘              └─────────────┘
```

The network is a collection of `Switch` objects connected by links.
Each link has:
- **Latency:** Cycles to traverse (configurable per-link)
- **Bandwidth:** Bytes per cycle (enforced by `Throttle`)

### The Switch: PerfectSwitch + Intermediate Buffers + Throttle

Each `Switch` (`src/mem/ruby/network/simple/Switch.hh`) contains two key components wired in sequence:

1. **PerfectSwitch** (`PerfectSwitch.cc`): Routes messages with a configurable routing latency
   - Reads from input MessageBuffers
   - Delegates destination lookup to a pluggable `BaseRoutingUnit` (default: `WeightBased`)
   - Enqueues message to intermediate buffers with `routing_latency` delay
   - "Perfect" refers to the absence of a pipeline — not to zero latency.
     Each Switch has separate `int_routing_latency` (for switch-to-switch hops) and `ext_routing_latency` (for switch-to-controller hops), both defaulting to 1 cycle (inherited from `BasicRouter.latency`).

2. **Throttle** (`Throttle.cc`): Enforces bandwidth constraints and adds link latency
   - Reads from intermediate buffers (placed there by PerfectSwitch)
   - Tracks remaining bandwidth budget per cycle
   - Delays messages if bandwidth exhausted this cycle
   - Enqueues to destination buffers with `link_latency` delay
   - One Throttle per output port

Between PerfectSwitch and Throttle sit **intermediate buffers** (`port_buffers`), one per (output port, vnet) pair.
These decouple routing from bandwidth enforcement and let both stages operate within the same simulation cycle using event priorities:
PerfectSwitch runs at `Default_Pri`, Throttle runs at `Default_Pri + 1`, guaranteeing that routing completes before bandwidth accounting begins.

```
┌──────────────────────────────────────────────────────────────────┐
│                             Switch                               │
│  ┌──────────┐    ┌───────────┐    ┌──────────────┐    ┌────────┐│
│  │  Input   │    │ Perfect   │    │ Intermediate │    │Throttle││
│  │ Buffers  │───▶│ Switch    │───▶│   Buffers    │───▶│  (BW + ││
│  │(per vnet)│    │(routing   │    │ (per port,   │    │  link  ││
│  │          │    │ latency)  │    │  per vnet)   │    │latency)││
│  └──────────┘    └───────────┘    └──────────────┘    └───┬────┘│
└───────────────────────────────────────────────────────────┼──────┘
                                                            │
                                                       To next switch
                                                       or controller
```

This architecture is fast because:
- No per-flit simulation — messages move as whole units
- Routing is a single table lookup, not a multi-stage pipeline
- Bandwidth accounting is coarse-grained (message-level, not flit-level)

What it does not model:
- No virtual channel allocation or VC contention
- No credit-based flow control or backpressure propagation
- No switch-arbitration contention between simultaneous arrivals
- No flit-level head-of-line blocking within a single port

However, SimpleNetwork is not entirely contention-free.
PerfectSwitch checks that all output ports have buffer space *before* dequeuing a message (`PerfectSwitch.cc:211–231`).
If any destination buffer is full, the message is held at the input — a form of output-port blocking.
The Throttle can also stall when its output buffer is full (`output_blocked`) or bandwidth is exhausted (`bw_saturated`), rescheduling itself for the next cycle.

### Routing and Starvation Prevention

**Routing** is delegated to a pluggable `BaseRoutingUnit`.
The default is `WeightBased` (`src/mem/ruby/network/simple/routing/WeightBased.cc`), which maintains output links sorted by `(order, weight, link_id)`.
When `adaptive_routing=True`, `WeightBased` recomputes each link's order based on its output queue depth, preferring less-congested paths.
For ordered virtual networks (where message ordering must be preserved), adaptive routing is disabled and the static weight order is used.

**Starvation prevention** is built into both PerfectSwitch and Throttle.
Each maintains a counter (`m_wakeups_wo_switch`) that increments every wakeup.
After `PRIORITY_SWITCH_LIMIT` (128) consecutive wakeups, the vnet processing order is inverted — if vnets were processed highest-first, they switch to lowest-first, and vice versa.
This prevents high-numbered vnets from perpetually starving low-numbered ones when bandwidth is scarce.

### Working Model: How a Message Travels

Let us trace a `GetS` message from L1 cache controller to directory controller in a Pt2Pt topology (one intermediate switch per controller, one internal link between them).

**Step 1: Enqueue at Source Controller**
- L1 controller's `out_port` MessageBuffer enqueues the message.
- The MessageBuffer schedules a wakeup for the PerfectSwitch in the source Switch.

**Step 2: PerfectSwitch Routing (source Switch)**
- PerfectSwitch wakes up, reads the input buffer.
- Calls `routing_unit.route()` to determine which output link(s) reach the directory's NodeID.
- Checks that the intermediate buffer for the chosen output port has space.
  If not, reschedules for the next cycle (output-port blocking).
- Dequeues the message and enqueues it into the intermediate buffer with `routing_latency` delay (default 1 cycle).

**Step 3: Throttle Transfer (source Switch)**
- The Throttle wakes up (same cycle, but at a lower event priority than PerfectSwitch).
- Reads the intermediate buffer.
  If the message is not yet ready (due to routing_latency), it will process it in a later cycle.
- Computes the message size: `MessageSizeType_to_int(msg_size) × MESSAGE_SIZE_MULTIPLIER` (where `MESSAGE_SIZE_MULTIPLIER = 1000`).
- Checks available bandwidth budget for this cycle (`getTotalLinkBandwidth()`).
- If budget is sufficient: dequeues from intermediate buffer, enqueues to the destination link buffer with `link_latency` delay.
- If budget is exhausted: sets `bw_saturated = true`, reschedules for next cycle.
  The message's remaining size (`units_remaining`) carries over.

**Step 4: Link Traversal**
- Message sits in the link buffer for `link_latency` cycles (default 1 cycle).
- After the delay, it becomes ready in the destination Switch's input buffer.

**Step 5: Destination Switch Processing**
- The destination Switch's PerfectSwitch routes the message to the appropriate external output port with `ext_routing_latency` delay.
- The destination Throttle transfers it to the controller's `in_port` buffer with another `link_latency` delay.

**Step 6: Controller Wakeup**
- Destination controller's `wakeup()` fires.
- Message is available via `peek()`.

For a Pt2Pt topology where source and destination are on different switches, the message traverses: 1 external link (controller → switch), 1 internal link (switch → switch), and 1 external link (switch → controller).
Each hop contributes its own routing_latency + link_latency.

```
Source   Switch 0                          Switch 1   Dir
Ctrl     PS      IntBuf  Throttle   Link   PS   Thr   Ctrl
─────────────────────────────────────────────────────────────
  ENQ─────▶ROUTE──▶[R]─────▶XFER────▶[L]──▶RT──▶XF──▶WAKE
           1cy     wait     +link_lat       1cy  +lat
                   for R               1cy

ENQ   = Controller enqueues to out_port
ROUTE = PerfectSwitch lookup + enqueue with routing_latency
[R]   = Message waits in intermediate buffer for routing_latency
XFER  = Throttle dequeues, enqueues with link_latency
[L]   = Message in transit for link_latency
RT    = Destination PerfectSwitch routes to ext output
XF    = Destination Throttle forwards to controller in_port
WAKE  = Controller wakeup
```

The total per-hop network latency (no contention) is:
$$L_{hop} = L_{routing} + L_{link}$$

With default parameters ($L_{routing} = 1$, $L_{link} = 1$), each hop costs 2 cycles.
In a Pt2Pt topology, an L1-to-Directory message crosses 3 hops (ext + int + ext), so the minimum latency is 6 cycles at defaults.

If bandwidth is insufficient to transmit the entire message in one cycle, the Throttle adds additional delay proportional to the message size.

---

## 9.3 Latency Accounting in SimpleNetwork

Understanding what SimpleNetwork does and does not model is critical for interpreting results.

### What SimpleNetwork Models

| Component | Implementation | Code Location |
|-----------|---------------|---------------|
| Routing latency | Per-switch configurable (`int_routing_latency`, `ext_routing_latency`, default 1 cycle each) | `Switch.cc:126` |
| Link latency | Per-link configurable (default 1 cycle) | `BasicLink.py`, `Throttle::operateVnet()` |
| Link bandwidth | Throttle enforces bandwidth budget per cycle | `Throttle::operateVnet()` |
| Virtual networks | Separate buffering per vnet | `MessageBuffer` per vnet |
| Routing | Pluggable via `BaseRoutingUnit`; default is `WeightBased` (table lookup sorted by weight) | `routing/WeightBased.cc` |
| Adaptive routing | Optional: `WeightBased(adaptive_routing=True)` reorders links by output queue depth | `WeightBased::route()` |
| Broadcast/multicast | Message cloned at switch, one copy per output link with trimmed destination set | `PerfectSwitch::operateMessageBuffer()` |
| Message ordering | Ordered buffer support per vnet | `MessageBuffer::m_ordered` |
| Output-port blocking | PerfectSwitch holds message if any output buffer is full | `PerfectSwitch.cc:211–231` |
| Statistics | Per-link utilization, message counts/bytes by type, bandwidth saturation cycles, stall cycles | `ThrottleStats`, `SwitchStats` |

### What SimpleNetwork Ignores

| Component | Why It Matters | Garnet Equivalent |
|-----------|---------------|-------------------|
| Flit-level timing | Messages are not split into flits; no head-of-line blocking within a port | `flit.hh`, router pipeline stages |
| Virtual channels | No VC allocation delays or VC exhaustion effects | `InputUnit` with configurable VCs per vnet |
| Credit flow control | No credit return latency, no backpressure propagation across multiple hops | `Credit` class, credit links |
| Router pipeline stages | No separate RC→VA→SA→ST→LT stage modeling | `Router.cc` per-stage implementations |
| Switch allocation contention | Multiple input ports do not arbitrate for the same crossbar path | `SwitchAllocator` |
| Link width constraints | No flit serialization based on physical link width | `NetworkLink` with `width` parameter |
| Wormhole/VC flow control interactions | No modeling of how partially transmitted packets block VCs | Garnet's credit-based wormhole switching |

### Formal: Latency Equation

A message from controller $A$ to controller $B$ traverses a sequence of hops.
Each hop passes through one Switch (PerfectSwitch + Throttle) and one link.
For $N$ hops:

$$L_{total} = \sum_{i=0}^{N-1} \left( L_{routing,i} + L_{link,i} + D_{throttle,i} \right)$$

Where:
- $L_{routing,i}$ = Routing latency at switch $i$ (default 1 cycle; `int_routing_latency` for internal hops, `ext_routing_latency` for external hops)
- $L_{link,i}$ = Link propagation latency on link $i$ (default 1 cycle, from `BasicLink.latency`)
- $D_{throttle,i}$ = Bandwidth-induced delay at switch $i$ output

The Throttle converts message size to an internal unit system:

$$S_{internal} = \text{MessageSizeType\_to\_int}(type) \times \text{MESSAGE\_SIZE\_MULTIPLIER}$$

where `MESSAGE_SIZE_MULTIPLIER = 1000`.
The per-cycle bandwidth budget is:

$$B_{cycle} = \text{endpoint\_bandwidth} \times \text{link\_bandwidth\_multiplier}$$

with defaults `endpoint_bandwidth = 1000` and `link_bandwidth_multiplier = 16` (from `BasicLink.bandwidth_factor`), yielding $B_{cycle} = 16000$ units per cycle.

The throttle delay for a single message in isolation is:

$$D_{throttle} = \max\left(0,\; \left\lceil \frac{S_{internal}}{B_{cycle}} \right\rceil - 1 \right)$$

A control message (8 bytes) requires $8 \times 1000 = 8000$ units, which fits in a single cycle ($B_{cycle} = 16000$), so $D_{throttle} = 0$.
A 72-byte data message (64B data + 8B control) requires $72000$ units, consuming $\lceil 72000 / 16000 \rceil = 5$ cycles of bandwidth, so $D_{throttle} = 4$ additional cycles.

When multiple messages compete for the same output port in the same cycle, they share the bandwidth budget sequentially.
The Throttle processes one vnet at a time within a wakeup, and if bandwidth runs out mid-cycle, it reschedules for the next cycle.

> **Deep Dive:** The Throttle tracks `units_remaining` per (vnet, channel) pair.
> If a large message cannot be fully "transmitted" in one cycle, the leftover units carry over to the next wakeup.
> During that time the message has *already* been enqueued to the output buffer with `link_latency` delay — the bandwidth accounting does not further delay its arrival.
> What it does delay is the *next* message on the same port: the Throttle will not dequeue another message until the current one's bandwidth is fully accounted for.

---

## 9.4 Topologies and Configuration

SimpleNetwork supports various topologies through Python configuration.
The topology determines how switches are connected and how messages are routed.

### Point-to-Point (Pt2Pt)

Every controller connects directly to every other controller via dedicated links.

```
┌─────────┐         ┌─────────┐
│  L1_0   │◀───────▶│  L1_1   │
│ Switch0 │         │ Switch1 │
└────┬────┘         └────┬────┘
     │                   │
     │     ┌─────────┐   │
     └────▶│  Dir 0  │◀──┘
           │ Switch2 │
           └────┬────┘
                │
           ┌────▼────┐
           │  Mem 0  │
           │ Switch3 │
           └─────────┘

Routers: N (one per controller)
Internal links: N*(N-1) unidirectional (fully connected)
External links: N (controller ↔ its router)
```

**Characteristics:**
- Minimum hop count between any two controllers (1 internal hop, plus external links at each end)
- Highest link count (quadratic in N)
- Routing is straightforward — each destination is directly reachable
- Unrealistic for large systems (link count grows as $O(N^2)$)

**Use for:** Small systems (2–8 nodes), protocol debugging, baseline comparison

### Crossbar

All controllers connect to a central switch that forwards messages.

```
          ┌──────────┐
     ┌───▶│  Switch  │◀───┐
     │    │(Crossbar)│    │
┌────┴───┐└────┬─────┘┌───┴────┐
│  L1_0  │     │      │  L1_1  │
│Switch 0│     │      │Switch 1│
└────────┘     │      └────────┘
               │
          ┌────┴────┐
          │  Dir 0  │
          │Switch 2 │
          └─────────┘

Routers: N+1 (one per controller + one central crossbar)
Internal links: 2*N (bidirectional between each controller router and crossbar)
External links: N
```

Note: each controller gets its own Switch because external links in SimpleNetwork do not model outgoing bandwidth — the per-controller Switch provides the Throttle needed for bandwidth enforcement on egress.

**Characteristics:**
- Constant internal hop count (every message goes controller → crossbar → destination, i.e. 2 internal hops)
- Linear link count
- Central crossbar switch is a potential bandwidth bottleneck under high load
- Simple routing (always through the central switch)

**Use for:** Medium systems, sanity checks, fairness studies

### Python Configuration

Topologies are defined in Python and instantiate the C++ network objects.

**Legacy path** (`configs/topologies/Crossbar.py`):

```python
class Crossbar(SimpleTopology):
    def makeTopology(self, options, network, IntLink, ExtLink, Router):
        # Create one router per controller + one crossbar
        routers = [Router(router_id=i) for i in range(len(self.nodes) + 1)]
        xbar = routers[len(self.nodes)]

        # External links: controllers to their routers
        ext_links = [ExtLink(link_id=i, ext_node=n, int_node=routers[i])
                     for (i, n) in enumerate(self.nodes)]

        # Internal links: controller routers to crossbar
        int_links = []
        for i in range(len(self.nodes)):
            int_links.append(IntLink(link_id=i, src_node=routers[i],
                                     dst_node=xbar, latency=link_latency))
            int_links.append(IntLink(link_id=i+len(self.nodes),
                                     src_node=xbar, dst_node=routers[i],
                                     latency=link_latency))
```

**Modern stdlib path** (`src/python/gem5/components/cachehierarchies/ruby/topologies/simple_pt2pt.py`):

```python
class SimplePt2Pt(SimpleNetwork):
    def connectControllers(self, controllers):
        # Create one router per controller
        self.routers = [Switch(router_id=i) for i in range(len(controllers))]

        # External links: controller to its router
        self.ext_links = [
            SimpleExtLink(link_id=i, ext_node=c, int_node=self.routers[i])
            for i, c in enumerate(controllers)
        ]

        # Internal links: every router to every other router
        link_count = 0
        int_links = []
        for ri in self.routers:
            for rj in self.routers:
                if ri != rj:
                    link_count += 1
                    int_links.append(
                        SimpleIntLink(link_id=link_count, src_node=ri,
                                      dst_node=rj)
                    )
        self.int_links = int_links
```

The key parameters are:

| Parameter | Default | Description |
|-----------|---------|-------------|
| `link_latency` (per link) | 1 cycle | Time for a message to traverse a link (applied by Throttle) |
| `int_routing_latency` (per switch) | 1 cycle | Routing delay for internal (switch-to-switch) hops |
| `ext_routing_latency` (per switch) | 1 cycle | Routing delay for external (switch-to-controller) hops |
| `endpoint_bandwidth` (network-wide) | 1000 | Unitless bandwidth scaling factor, multiplied with per-link `bandwidth_factor` |
| `bandwidth_factor` (per link) | 16 | Per-link bandwidth multiplier; effective BW = `endpoint_bandwidth × bandwidth_factor` |
| `buffer_size` (network-wide) | 0 | Internal buffer capacity per port; 0 means infinite |
| `physical_vnets_channels` | `[]` | Per-vnet channel counts; empty means all vnets share one channel |

> **Deep Dive: Physical VNets Mode.**
> By default, all virtual networks share a single bandwidth pool at each Throttle.
> One busy vnet can consume all available bandwidth in a cycle, starving other vnets.
> Setting `physical_vnets_channels` (e.g., `[1, 1, 1]` for 3 vnets) gives each vnet its own independent bandwidth pool.
> Combined with `physical_vnets_bandwidth`, this lets you model separate physical channels for request, response, and data networks — a common feature in real interconnects like AMBA CHI.
> Enable via `--simple-physical-channels` on the legacy command line.

---

## 9.5 Running SimpleNetwork Experiments

Let us run concrete experiments with SimpleNetwork-backed Ruby systems.

### Legacy Path with MI_example

The legacy Ruby configuration uses command-line options to select the network:

```bash
# Build gem5 with Ruby (MI_example protocol, the default for RISCV)
scons build/RISCV/gem5.opt -j$(nproc)

# Run with SimpleNetwork and Crossbar topology
./build/RISCV/gem5.opt -d m5out/simple-crossbar \
    configs/example/ruby_random_test.py \
    --protocol=MI_example \
    --network=simple \
    --topology=Crossbar \
    --link-latency=2 \
    --num-cpus=4
```

**Key statistics to observe:**

```
# Per-switch aggregate statistics
system.ruby.network.switches0.percent_links_utilized   # Average utilization across all output ports
system.ruby.network.switches0.msg_count.Response_Data  # Data response messages through this switch
system.ruby.network.switches0.msg_count.Request_Control # Control requests through this switch

# Per-throttle (per output port) statistics
system.ruby.network.switches0.throttle00.link_utilization   # Utilization of this specific output port (%)
system.ruby.network.switches0.throttle00.avg_bandwidth      # Average bandwidth (GB/s)
system.ruby.network.switches0.throttle00.avg_useful_bandwidth  # Data-only bandwidth (GB/s)
system.ruby.network.switches0.throttle00.total_msg_count    # Total messages through this port
system.ruby.network.switches0.throttle00.total_bw_sat_cy    # Cycles where bandwidth was saturated
system.ruby.network.switches0.throttle00.total_stall_cy     # Cycles where output was blocked
system.ruby.network.switches0.throttle00.avg_msg_wait_time  # Average message latency (ticks)
```

### Stdlib Path with SimplePt2Pt

The modern stdlib provides a cleaner interface:

```python
from gem5.components.cachehierarchies.ruby.mi_example_cache_hierarchy import (
    MIExampleCacheHierarchy,
)
from gem5.components.memory import SimpleMemory
from gem5.components.processors import SimpleProcessor
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.boards import SimpleBoard
from gem5.isas import ISA
from gem5.resources.resource import CustomResource
from gem5.simulate.simulator import Simulator

# Create the Ruby hierarchy with SimplePt2Pt network
cache_hierarchy = MIExampleCacheHierarchy(
    size="32KiB",  # L1 size
    assoc=8,
    network_type=SimplePt2Pt,  # SimpleNetwork-based topology
)

memory = SimpleMemory(latency="100ns", bandwidth="10GiB/s")
cpu = SimpleProcessor(cpu_type=CPUTypes.TIMING, num_cores=4, isa=ISA.RISCV)

board = SimpleBoard(
    clk_freq="1GHz",
    processor=cpu,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

# Set workload
board.set_se_binary_workload(CustomResource("path/to/binary"))

# Run
simulator = Simulator(board=board)
simulator.run()
```

**Comparing SimpleNetwork vs. Garnet:**

```python
# For Garnet, change network_type (if supported by the hierarchy)
# or use the legacy path with --network=garnet

cache_hierarchy = MIExampleCacheHierarchy(
    size="32KiB",
    assoc=8,
    network_type=SimplePt2Pt,  # SimpleNetwork
    # For Garnet, the hierarchy may need different parameters
)
```

### Experiment: Measuring Network Latency

Create a minimal test to isolate network latency:

```python
# test_network_latency.py
from m5.objects import *

# Create minimal system with two controllers
system = System()
system.clk_domain = SrcClockDomain(clock="1GHz")
system.mem_mode = 'timing'

# Create Ruby system with SimpleNetwork
system.ruby = RubySystem()
network = SimpleNetwork(
    ruby_system=system.ruby,
    topology="Pt2Pt",
    routers=[],
    ext_links=[],
    int_links=[],
)

# ... (controller setup)

# Vary link latency and measure end-to-end request latency
for lat in [1, 2, 5, 10]:
    # Reconfigure network
    for link in network.int_links:
        link.latency = lat
    # Run experiment
    # Measure: system.ruby.l1_cntrl0.L1cache.m_latency_hist
```

---

## 9.6 Failure Modes: When Abstraction Misleads

Using SimpleNetwork incorrectly leads to systematic errors in interpretation.

### Conflating Routing Latency with Link Latency

**The trap:** You read that real router pipelines take 3–4 cycles.
You set `link_latency=4` to compensate, leaving `routing_latency` at its default of 1.

**The problem:** SimpleNetwork has *two* separate latency parameters per hop: `routing_latency` (applied by PerfectSwitch) and `link_latency` (applied by Throttle).
Inflating `link_latency` to compensate for missing router pipeline detail changes the latency distribution incorrectly.

In a 4×4 mesh, a corner-to-corner message traverses 6 hops.
With `link_latency=4` and default `routing_latency=1`:
- Modeled: $6 \times (1 + 4) = 30$ cycles per direction
- A more realistic breakdown might be: $6 \times (3 + 1) = 24$ cycles (3-cycle router, 1-cycle link)

These look similar in total but have different sensitivity to hop count changes.
If you later change the topology, the error compounds differently.

**The fix:** If router pipeline depth matters for your study, use Garnet, which models each stage explicitly.
If you must use SimpleNetwork and want a rough approximation, set `routing_latency` to represent router delay and `link_latency` to represent wire delay — but recognize this is still a simplification.

### Underestimating Contention Effects

**The trap:** SimpleNetwork shows low, stable latency under moderate load.
You conclude the network is not a bottleneck.

**The problem:** SimpleNetwork does model some contention — Throttle bandwidth saturation and output-port blocking — but it misses the contention mechanisms that dominate in real networks at higher loads:
- Flit-level head-of-line blocking (a long message blocks shorter ones behind it in the same VC)
- VC exhaustion (all VCs consumed, blocking new packets from entering)
- Credit backpressure propagation (congestion at one router stalls upstream routers)
- Switch arbitration delays (multiple packets competing for the same crossbar path)

Under high load, real networks exhibit a sharp latency knee at the saturation point.
SimpleNetwork's latency increases more gradually because its contention model is coarser.

**The symptom:** Your results show protocol improvements under high load, but the network latency curve looks unrealistically smooth — no saturation knee, no latency explosion.

**The fix:** For saturation studies, use `configs/example/garnet_synth_traffic.py` with Garnet.
Compare latency-throughput curves from both models to understand where they diverge.

### Protocol vs. Network Attribution

**The trap:** You observe 150-cycle average miss latency.
You optimize the protocol, reducing it to 120 cycles.

**The problem:** Was the 30-cycle improvement from:
- Fewer protocol messages?
- Shorter network paths?
- Better message scheduling?

With SimpleNetwork, you cannot distinguish network effects from protocol effects because the network model is too simple.

**The fix:** Add instrumentation to break down latency:

```cpp
// In your SLICC protocol
transition(I, GetS, S) {
  // ...
  recordNetworkLatency(curTick() - request_entry_time);
  // ...
}
```

Or use Ruby's built-in profiler (`src/mem/ruby/profiler/Profiler.cc`) to track message latencies per virtual network.

---

## 9.7 How We Know This

The SimpleNetwork behavior is documented in:

1. **Source code:**
   - `src/mem/ruby/network/simple/SimpleNetwork.{hh,cc}` — Network construction, link wiring
   - `src/mem/ruby/network/simple/Switch.{hh,cc}` — Switch assembly: PerfectSwitch + intermediate buffers + Throttle
   - `src/mem/ruby/network/simple/PerfectSwitch.cc` — Routing with configurable latency, output-port blocking, priority inversion
   - `src/mem/ruby/network/simple/Throttle.{hh,cc}` — Bandwidth enforcement, link latency, message size accounting
   - `src/mem/ruby/network/simple/routing/WeightBased.{hh,cc}` — Default routing unit with optional adaptive routing

2. **Key invariants verified by reading the code:**
   - PerfectSwitch adds `routing_latency` (not zero) when enqueuing to intermediate buffers (`Switch.cc:126–130`, `PerfectSwitch.cc:270–272`)
   - Throttle adds `link_latency` when enqueuing to destination buffers (`Throttle.cc:203–204`)
   - Bandwidth budget is recalculated fresh each wakeup as `getTotalLinkBandwidth()` (`Throttle.cc:252`)
   - `units_remaining` carries over across cycles for partially transmitted messages (`Throttle.cc:175–227`)
   - Both PerfectSwitch and Throttle invert vnet processing order every `PRIORITY_SWITCH_LIMIT` (128) wakeups to prevent starvation (`PerfectSwitch.cc:287–292`, `Throttle.cc:263–266`)
   - Event priorities ensure PerfectSwitch runs before Throttle within the same cycle (`Switch.hh:PERFECTSWITCH_EV_PRI`, `THROTTLE_EV_PRI`)

3. **Validation approach:**
   - Run identical experiments with SimpleNetwork vs. Garnet
   - Compare latency distributions at low load (should be close)
   - Compare latency under increasing load (Garnet shows saturation knee; SimpleNetwork does not exhibit the same sharp transition)

4. **Parameters verified via:**
   - `configs/network/Network.py` — CLI option definitions and defaults
   - `src/mem/ruby/network/simple/SimpleNetwork.py` — SimObject parameter definitions (buffer_size, endpoint_bandwidth, physical_vnets_channels)
   - `src/mem/ruby/network/BasicLink.py` — Link latency (default 1) and bandwidth_factor (default 16)
   - `configs/topologies/*.py` — Topology implementations (Crossbar, Pt2Pt)

---

## Key Ideas

1. **SimpleNetwork models message transport, not router microarchitecture.**
   It is fast but abstracts away flits, VCs, and credit flow control.

2. **Per-hop latency = routing_latency + link_latency + throttle_delay.**
   Each Switch has configurable `int_routing_latency` and `ext_routing_latency` (default 1 cycle each).
   Each link has a configurable `latency` (default 1 cycle).
   With no bandwidth contention, each hop costs 2 cycles at default settings.

3. **Bandwidth is enforced at output Throttles using a budget system.**
   Message sizes are scaled by `MESSAGE_SIZE_MULTIPLIER` (1000) and consumed from a per-cycle budget of `endpoint_bandwidth × link_bandwidth_multiplier`.
   Large data messages can span multiple cycles of bandwidth, delaying subsequent messages on the same port.

4. **Each Switch has three stages: PerfectSwitch → intermediate buffers → Throttle.**
   PerfectSwitch handles routing, Throttle handles bandwidth and link latency.
   Event priorities ensure they execute in order within the same cycle.

5. **SimpleNetwork is appropriate for:**
   - Protocol correctness testing
   - Coherence algorithm comparison
   - Studies where protocol-level traffic patterns dominate over network microarchitecture
   - Fast simulation when the network is not the primary focus

6. **SimpleNetwork is inappropriate for:**
   - NoC architecture studies (router pipeline, buffer sizing)
   - Routing algorithm evaluation under contention
   - Saturation behavior and latency-throughput curve analysis
   - Any study where VC allocation, credit backpressure, or flit-level timing matters

---

## 1-Page Mental Model

**SimpleNetwork in one page:**

Ruby controllers communicate via MessageBuffers.
SimpleNetwork connects these buffers through a graph of Switches linked together.
Each Switch contains three stages: PerfectSwitch (routing lookup + routing_latency), intermediate buffers, and Throttle (bandwidth enforcement + link_latency).

A message travels: Controller out_port → Switch input → PerfectSwitch routes (adds routing_latency) → intermediate buffer → Throttle transfers (adds link_latency, checks bandwidth) → next Switch input → … → Controller in_port.

Each hop costs `routing_latency + link_latency` cycles with no contention (2 cycles at defaults).
Bandwidth is enforced at Throttles using a per-cycle budget.
Large messages can take multiple cycles of bandwidth, blocking subsequent messages on the same port.
Messages move as whole units — there are no flits, no VC allocation, and no credit backpressure.

Use SimpleNetwork when you care about protocol behavior, message counts, and message paths.
Use Garnet when VC contention, flit-level timing, and router pipeline details matter.

---

## Common Misconceptions

| Misconception | Reality |
|--------------|---------|
| "SimpleNetwork has zero router latency — routing is instant" | Each Switch adds `routing_latency` (default 1 cycle). PerfectSwitch is "perfect" in that it has no pipeline contention, not that it is zero-latency. |
| "Link latency is the only latency in SimpleNetwork" | Per-hop latency = `routing_latency` + `link_latency`. Both default to 1 cycle. A single hop costs 2 cycles, not 1. |
| "SimpleNetwork has no contention at all" | It lacks flit-level contention, but PerfectSwitch blocks when output buffers are full, and Throttle stalls when bandwidth is exhausted. These are coarser forms of contention. |
| "SimpleNetwork cannot model bandwidth" | It can — via Throttle. But bandwidth is modeled at message granularity, not flit-by-flit. |
| "More virtual networks improve throughput" | In SimpleNetwork without `physical_vnets_channels`, vnets share a single bandwidth pool. Adding vnets does not add bandwidth — it only provides protocol-level separation. |
| "endpoint_bandwidth is in bytes per cycle" | It is a unitless multiplier. Effective bandwidth = `endpoint_bandwidth × link_bandwidth_multiplier`. The default combination (1000 × 16 = 16000) determines how many internal bandwidth units are available per cycle. |
| "I should always use Garnet for accuracy" | Garnet is slower to simulate. Use the simplest model sufficient for your research question. If you are studying protocol behavior and not network microarchitecture, SimpleNetwork is appropriate. |

---

## If You Remember One Thing

**SimpleNetwork is a message-level bandwidth-latency model, not a cycle-accurate router model.**

It tells you how long messages take to travel (routing_latency + link_latency per hop) and how much bandwidth they consume (Throttle budget accounting).
It does not model flit-level pipeline stages, virtual channel allocation, or credit-based flow control.

If your research question depends on how packets compete for router resources — use Garnet.
If your research question depends on protocol-level message counts and paths — SimpleNetwork is sufficient and faster.

---

## Exercises

1. **Baseline Comparison**
   Run the same workload (e.g., `ruby_random_test.py`) with `--network=simple` and `--network=garnet`.
   Compare `system.ruby.network.msg_count.*` statistics.
   Are they identical? Why or why not?

2. **Link Latency Sweep**
   Using SimpleNetwork with Pt2Pt topology, vary `--link-latency` from 1 to 10.
   Plot average miss latency vs. link latency.
   Is the relationship linear? Should it be?

3. **Bandwidth Saturation**
   Increase the injection rate in `ruby_random_test.py` until latency spikes.
   Compare the onset of saturation between SimpleNetwork and Garnet.
   How do the latency-throughput curves differ?

4. **Topology Impact**
   Compare Pt2Pt vs. Crossbar topologies with SimpleNetwork for a 16-core system.
   Measure:
   - Average message latency
   - Maximum link utilization
   - Simulation speed (simulated cycles per second)

   When does the crossbar become a bottleneck?

5. **Protocol vs. Network**
   Run MESI_Two_Level with SimpleNetwork.
   Modify the protocol to send one extra message per miss (artificially).
   How much does total runtime increase?
   Is the increase proportional to the extra messages?
   What does this tell you about SimpleNetwork's modeling assumptions?

6. **Throttle Deep Dive**
   Read `src/mem/ruby/network/simple/Throttle.cc`.
   Trace how `m_units_remaining[vnet][channel]` is set from `network_message_to_size()` and decremented in `operateVnet()`.
   What happens when `bw_saturated` is set to true?
   When is `output_blocked` set, and how does it differ from `bw_saturated`?
   Enable `physical_vnets_channels` for a 3-vnet protocol and observe how the bandwidth distribution changes compared to the default shared-pool mode.

---

*Next: Chapter 10 covers Garnet 3.0—the cycle-accurate network model with flits, VCs, credits, and detailed router pipelines.*
