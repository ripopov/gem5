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
  - [The Switch: Perfect Switch + Throttle](#the-switch-perfect-switch--throttle)
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
  - [Mistaking Link Latency for Router Latency](#mistaking-link-latency-for-router-latency)
  - [Ignoring Contention](#ignoring-contention)
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

`SimpleNetwork` collapses all of this into: enqueue delay + link latency + throttle bandwidth check.
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
│  • Messages move atomically                                     │
│  • Latency = enqueue + link + throttle                          │
│  • No flits, no VCs, no credit flow control                     │
│  • Bandwidth enforced at output                                 │
│                                                                 │
│  Use when: protocol studies, traffic reduction, functional     │
│  validation                                                     │
├─────────────────────────────────────────────────────────────────┤
│  Garnet (Flit-Level, Cycle-Accurate)                            │
│  ───────────────────────────────────                            │
│  • Packets split into flits                                     │
│  • Router pipeline: RC → VA → SA → ST → LT                      │
│  • Credit-based flow control                                    │
│  • Contention at every stage                                    │
│                                                                 │
│  Use when: NoC architecture, routing, saturation studies       │
└─────────────────────────────────────────────────────────────────┘
```

The key insight: **SimpleNetwork models bandwidth and latency, not contention dynamics.**

---

## 9.2 SimpleNetwork Architecture

`SimpleNetwork` (`src/mem/ruby/network/simple/SimpleNetwork.hh`) inherits from the abstract `Network` base class.
It provides message transport between Ruby controllers without modeling individual router pipelines.

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

### The Switch: Perfect Switch + Throttle

Each `Switch` (`src/mem/ruby/network/simple/Switch.hh`) contains two key components:

1. **PerfectSwitch:** Routes messages without delay (hence "perfect")
   - Reads from input MessageBuffers
   - Looks up destination in routing table
   - Places message in output buffer immediately
   - No cycle-by-cycle pipeline modeling

2. **Throttle:** Enforces bandwidth constraints
   - Sits at output ports
   - Tracks available bandwidth per virtual network
   - Delays messages if bandwidth exhausted
   - Accounts for message size vs. link width

```
┌─────────────────────────────────────────────────────────┐
│                      Switch                             │
│  ┌─────────────┐      ┌──────────┐     ┌───────────┐   │
│  │   Input     │      │ Perfect  │     │  Output   │   │
│  │  Buffers    │─────▶│ Switch   │────▶│  Buffers  │   │
│  │  (per vnet) │      │(0 cycles)│     │ (per port)│   │
│  └─────────────┘      └──────────┘     └─────┬─────┘   │
│                                               │         │
│                                         ┌─────▼─────┐   │
│                                         │ Throttle  │   │
│                                         │(bandwidth)│   │
│                                         └─────┬─────┘   │
│                                               │         │
└───────────────────────────────────────────────┼─────────┘
                                                │
                                           To next switch
                                           or controller
```

This architecture is fast because:
- Routing happens in one "cycle" (event)
- No per-flit simulation overhead
- Bandwidth accounting is coarse-grained

But it is unrealistic because:
- No head-of-line blocking within routers
- No credit backpressure propagation
- No contention for switch resources
- No virtual channel allocation delays

### Working Model: How a Message Travels

Let us trace a `GetS` message from L1 cache to directory:

**Step 1: Enqueue at Source**
- L1 controller's `out_port` buffer enqueues the message
- Message has `enqueue_time = curTick + link_latency`

**Step 2: PerfectSwitch Routing**
- Switch wakes up, reads input buffer
- Looks up destination (directory NodeID) in routing table
- For a Pt2Pt network: direct route to destination switch
- For a Crossbar: route to central crossbar switch

**Step 3: Throttle Bandwidth Check**
- Throttle calculates message size (control vs. data)
- Checks available bandwidth for the virtual network
- If bandwidth available: message passes immediately
- If bandwidth exhausted: message delayed until next "quota"

**Step 4: Link Traversal**
- Message arrives at destination switch after `link_latency` cycles
- Enqueued in destination controller's `in_port` buffer

**Step 5: Controller Wakeup**
- Destination controller's `wakeup()` scheduled
- Message available via `peek()` in next cycle

```
Cycle   0      1      2      3      4      5      6
L1      ENQ───┐
Switch        PERF───┐
Throttle             THR───┐
Link                        LINK───┐
Dir                                INQ───WAKE

ENQ   = Enqueue to output buffer
PERF  = PerfectSwitch routing (instant)
THR   = Throttle bandwidth check
LINK  = Link traversal (link_latency cycles)
INQ   = Enqueue to input buffer
WAKE  = Controller wakeup
```

The total network latency is:
$$NetworkLatency = LinkLatency_{L1 \to Dir} + ThrottleDelay$$

With no bandwidth contention, $ThrottleDelay = 0$.

---

## 9.3 Latency Accounting in SimpleNetwork

Understanding what SimpleNetwork does and does not model is critical for interpreting results.

### What SimpleNetwork Models

| Component | Implementation | Code Location |
|-----------|---------------|---------------|
| Link latency | Per-link configurable latency | `SimpleLink` params |
| Link bandwidth | Throttle enforces bytes/cycle | `Throttle::operateVnet()` |
| Virtual networks | Separate buffering per vnet | `MessageBuffer` per vnet |
| Routing | Table-based destination lookup | `BaseRoutingUnit` |
| Broadcast/multicast | Message duplication at switch | `PerfectSwitch::processMessage()` |
| Message ordering | Ordered buffer support | `MessageBuffer::m_ordered` |
| Statistics | Per-link bandwidth utilization | `ThrottleStats` |

### What SimpleNetwork Ignores

| Component | Why It Matters | Garnet Equivalent |
|-----------|---------------|-------------------|
| Flit-level timing | Head-of-line blocking, pipelining | `flit.hh`, router pipeline stages |
| Virtual channels | VC allocation, VC congestion | `InputUnit` with multiple VCs |
| Credit flow control | Backpressure propagation | `Credit` class, credit links |
| Router pipeline stages | RC→VA→SA→ST→LT delays | `Router.cc` stage implementations |
| Switch allocation contention | Multiple packets compete | `SwitchAllocator` |
| Link width constraints | Flit serialization | `NetworkLink` with width params |

### Formal: Latency Equation

For a message traveling from controller $A$ to controller $B$ through $N$ switches:

$$L_{total} = \sum_{i=0}^{N-1} \left( L_{link,i} + D_{throttle,i} \right)$$

Where:
- $L_{link,i}$ = Configured latency of link $i$ (default 1 cycle)
- $D_{throttle,i}$ = Bandwidth-induced delay at switch $i$ output

The throttle delay is calculated as:

$$D_{throttle} = \max\left(0, \frac{S_{msg} - B_{available}}{B_{per\_cycle}}\right)$$

Where:
- $S_{msg}$ = Message size in bytes
- $B_{available}$ = Remaining bandwidth quota this cycle
- $B_{per\_cycle}$ = Link bandwidth (bytes/cycle)

**Key limitation:** This is work-conserving with coarse granularity.
If bandwidth is available, the message goes immediately—there is no modeling of head-of-line blocking from other messages already in flight.

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

Links: N*(N-1) for N controllers (fully connected)
```

**Characteristics:**
- Lowest latency (always 1 hop)
- Highest link count (quadratic in N)
- No routing decisions needed
- Unrealistic for large systems

**Use for:** Small systems (2-8 nodes), protocol debugging, baseline comparison

### Crossbar

All controllers connect to a central switch that forwards messages.

```
          ┌─────────┐
     ┌───▶│ Switch  │◀───┐
     │    │(Crossb)│    │
┌────┴───┐└────┬────┘┌───┴────┐
│  L1_0  │     │     │  L1_1  │
│Switch 0│     │     │Switch 1│
└────────┘     │     └────────┘
               │
          ┌────┴────┐
          │  Dir 0  │
          │Switch 2 │
          └─────────┘

Links: 2*N (each controller to/from crossbar)
```

**Characteristics:**
- Constant hop count (2 hops max)
- Linear link count
- Central switch is potential bottleneck
- Simple routing (always through crossbar)

**Use for:** Medium systems (8-32 nodes), sanity checks, fairness studies

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
- `link_latency`: Cycles to traverse a link (default 1)
- `router_latency`: Not used by SimpleNetwork (only Garnet)
- `endpoint_bandwidth`: Bytes per cycle per endpoint

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
system.ruby.network.msg_count.Control        # Control messages
system.ruby.network.msg_count.Data           # Data messages
system.ruby.network.msg_byte.Control         # Control bytes
system.ruby.network.msg_byte.Data            # Data bytes

system.ruby.network.switches0.throttle0.link_utilization  # Per-link utilization
system.ruby.network.switches0.throttle0.avg_bandwidth     # Bytes/cycle
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

### Mistaking Link Latency for Router Latency

**The trap:** You read that router pipelines take 3-4 cycles in real systems.
You set `link_latency=4` to compensate.

**The problem:** This conflates two different phenomena.
- Router latency affects every hop
- Link latency affects every link

In a 4x4 mesh, a corner-to-corner message traverses 6 links and 5 routers.
If you set `link_latency=4` to approximate router delays:
- Actual modeled: $6 \times 4 = 24$ cycles (too high)
- Should be: $6 \times 1 + 5 \times 3 = 21$ cycles (different distribution)

**The fix:** Use Garnet if router pipeline stages matter for your study.

### Ignoring Contention

**The trap:** SimpleNetwork shows low latency under light load.
You conclude the network is not a bottleneck.

**The problem:** SimpleNetwork's throttle model is optimistic.
It does not model:
- Head-of-line blocking
- VC exhaustion
- Credit backpressure delays

Under high load, real networks exhibit latency spikes and saturation.
SimpleNetwork shows gradual linear increase at best.

**The symptom:** Your results show protocol improvements but the network never saturates, even at unrealistic injection rates.

**The fix:** For saturation studies, use `garnet_synth_traffic.py` with Garnet.

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
   - `src/mem/ruby/network/simple/SimpleNetwork.{hh,cc}` — Main network class
   - `src/mem/ruby/network/simple/Switch.{hh,cc}` — Switch with PerfectSwitch + Throttle
   - `src/mem/ruby/network/simple/PerfectSwitch.cc` — Zero-latency routing
   - `src/mem/ruby/network/simple/Throttle.{hh,cc}` — Bandwidth enforcement

2. **Key invariants:**
   - `PerfectSwitch` processes messages in priority order (`PRIORITY_SWITCH_LIMIT` = 128)
   - `Throttle` bandwidth quota resets every cycle (work-conserving)
   - Link latency added via `scheduleEventAbsolute()` in `operateVnet()`

3. **Validation approach:**
   - Run identical experiments with SimpleNetwork vs. Garnet
   - Compare latency distributions at low load (should match)
   - Compare latency at high load (Garnet shows saturation, SimpleNetwork does not)

4. **Parameters verified via:**
   - `configs/network/Network.py` — CLI option definitions
   - `src/mem/ruby/network/simple/SimpleNetwork.py` — SimObject parameters
   - `configs/topologies/*.py` — Topology implementations

---

## Key Ideas

1. **SimpleNetwork models message transport, not router microarchitecture.**
   It is fast but abstracts away flits, VCs, and credit flow control.

2. **Latency in SimpleNetwork = link_latency + throttle delay.**
   There is no separate router latency parameter because routing is instantaneous.

3. **Bandwidth is enforced at output throttles, not links.**
   The throttle tracks bytes sent per cycle and delays messages if quota exceeded.

4. **SimpleNetwork is appropriate for:**
   - Protocol correctness testing
   - Coherence algorithm comparison
   - Low-load latency studies
   - Fast simulation when network is not the focus

5. **SimpleNetwork is inappropriate for:**
   - NoC architecture studies
   - Routing algorithm evaluation
   - Saturation behavior analysis
   - Any study where contention dynamics matter

---

## 1-Page Mental Model

**SimpleNetwork in one page:**

Ruby controllers communicate via MessageBuffers.
SimpleNetwork connects these buffers through a network of Switches.
Each Switch contains a PerfectSwitch (instant routing) and Throttles (bandwidth enforcement).

A message travels: Controller out_port → Switch input → PerfectSwitch routes → Throttle checks bandwidth → Link latency → Destination Switch → Controller in_port.

Link latency is configurable per-link (default 1 cycle).
Bandwidth is enforced per virtual network at output throttles.
Messages move atomically—there are no flits, no VC allocation, and no credit backpressure.

Use SimpleNetwork when you care about protocol behavior, not network contention.
Use Garnet when every flit, VC, and router stage matters.

---

## Common Misconceptions

| Misconception | Reality |
|--------------|---------|
| "Link latency includes router delay" | No—SimpleNetwork has zero router latency. Set link_latency to model wire delay only. |
| "SimpleNetwork cannot model bandwidth" | Yes it can—via Throttle. But it models bandwidth coarsely, not with flits. |
| "More virtual networks improve performance" | In SimpleNetwork, virtual networks prevent deadlock but do not improve throughput—there is no VC contention model. |
| "SimpleNetwork and Garnet give similar results at low load" | Yes, for latency. But bandwidth-limited behavior diverges even at moderate load. |
| "I should always use Garnet for accuracy" | No—Garnet is 3-5x slower. Use the simplest model sufficient for your research question. |

---

## If You Remember One Thing

**SimpleNetwork is a bandwidth-latency model, not a contention model.**

It tells you how long messages take to travel and how much bandwidth they consume.
It does not tell you what happens when multiple packets compete for the same router resources at the same time.

If your research question depends on contention dynamics—use Garnet.
If your research question depends on message counts and paths—SimpleNetwork is sufficient and faster.

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
   Trace how `m_units_remaining` is calculated and decremented.
   What happens when `bw_saturated` is true?
   How does physical_vnets mode differ from the default?

---

*Next: Chapter 10 covers Garnet 3.0—the cycle-accurate network model with flits, VCs, credits, and detailed router pipelines.*
