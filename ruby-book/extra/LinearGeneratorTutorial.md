# The gem5 LinearGenerator: End-to-End Deep Dive

**Audience:** C++ / SystemC experts who are new to the gem5 codebase.
This document is self-contained — you do not need to read anything else first.

---

## Table of Contents

1. [What Problem Does the LinearGenerator Solve?](#1-what-problem-does-the-lineargenerator-solve)
2. [gem5 Fundamentals for SystemC Engineers](#2-gem5-fundamentals-for-systemc-engineers)
3. [Architecture Overview](#3-architecture-overview)
4. [Layer 1 — Python Configuration Layer](#4-layer-1--python-configuration-layer)
5. [Layer 2 — The Python-to-C++ Bridge (PyTrafficGen)](#5-layer-2--the-python-to-c-bridge-pytrafficgen)
6. [Layer 3 — The C++ Simulation Engine (BaseTrafficGen)](#6-layer-3--the-c-simulation-engine-basetrafficgen)
7. [Layer 4 — The LinearGen Packet Factory](#7-layer-4--the-lineargen-packet-factory)
8. [End-to-End Packet Lifecycle](#8-end-to-end-packet-lifecycle)
9. [Timing Model and Rate Control](#9-timing-model-and-rate-control)
10. [Back-Pressure and Retry Protocol](#10-back-pressure-and-retry-protocol)
11. [Statistics Collection](#11-statistics-collection)
12. [Complete File Map](#12-complete-file-map)
13. [Worked Example with Numerical Values](#13-worked-example-with-numerical-values)
14. [Common Pitfalls and Debugging](#14-common-pitfalls-and-debugging)

---

## 1. What Problem Does the LinearGenerator Solve?

When developing or validating a memory subsystem (caches, coherence protocols, NoCs, DRAM controllers), you often need reproducible, parameterized traffic **without** running a real CPU or application binary.
The LinearGenerator is gem5's built-in sequential-address traffic injector: it walks linearly through an address range, issuing read and write [`Packet`](../../src/mem/packet.hh#L294)s at a configurable rate, for a configurable duration.

Think of it as a **verification IP block** — like a SystemC `sc_module` that drives TLM transactions into your DUT, except the "DUT" is gem5's entire memory hierarchy.

**Typical use cases:**

- Measuring cache hit/miss rates for a known access pattern
- Stress-testing a coherence protocol with multi-core linear sweeps
- Benchmarking DRAM controller throughput with controlled request rates
- Validating interconnect (crossbar/NoC) under deterministic traffic

---

## 2. gem5 Fundamentals for SystemC Engineers

Before diving into the generator code, you need a mental model of four gem5 concepts that have direct SystemC equivalents.

### 2.1 SimObject ≈ `sc_module`

Every hardware component in gem5 (CPU, cache, memory controller, traffic generator) inherits from [`SimObject`](../../src/sim/sim_object.hh#L146).
Like `sc_module`, it has a name, a hierarchical parent, and an initialization lifecycle.
Unlike SystemC, SimObject configuration is done in **Python** (not C++ constructors), and the C++ object is instantiated by the framework from a generated `Params` struct.

**Initialization order** (called automatically by the framework):
```
init() → regStats() → initState()/loadState() → resetStats() → startup() → drainResume()
```

### 2.2 Ports ≈ TLM Initiator/Target Sockets

gem5 connects components using **directional port pairs**:

| gem5 | SystemC/TLM |
|------|-------------|
| [`RequestPort`](../../src/mem/port.hh#L134) | `tlm_initiator_socket` (sends requests, receives responses) |
| [`ResponsePort`](../../src/mem/port.hh#L347) | `tlm_target_socket` (receives requests, sends responses) |

A `RequestPort` calls `sendTimingReq(pkt)` to push a packet downstream.
The peer `ResponsePort` receives it in `recvTimingReq(pkt)`.
Responses flow the reverse direction: `ResponsePort::sendTimingResp(pkt)` → `RequestPort::recvTimingResp(pkt)`.

**Key difference from TLM:** gem5 ports have an explicit **retry protocol** (see Section 10).
If `sendTimingReq()` returns `false`, the sender must hold the packet and wait for a `recvReqRetry()` callback.
This is like `tlm_sync_enum::TLM_ACCEPTED` vs. blocking — but with an explicit handshake.

### 2.3 Events ≈ `sc_event` + `notify()`

gem5 is a discrete-event simulator.
Instead of `SC_METHOD` sensitivity lists, you explicitly **schedule events at absolute ticks**:

```cpp
// In SystemC:
//   SC_METHOD(update);  sensitive << clock.pos();
//
// In gem5:
EventFunctionWrapper updateEvent([this]{ update(); }, "name");
schedule(updateEvent, curTick() + delay);  // absolute tick
```

`curTick()` returns the current simulation time in **ticks** (the smallest time unit; typically 1 tick = 1 picosecond, configurable).

[`EventFunctionWrapper`](../../src/sim/eventq.hh#L1136) wraps any `std::function<void()>` into a schedulable event.
When the event fires, `process()` calls your lambda.

### 2.4 Packet ≈ TLM Generic Payload

A [`Packet`](../../src/mem/packet.hh#L294) carries a memory transaction through the hierarchy:

```
Packet
  ├── req: RequestPtr        // persistent metadata (address, size, flags, requestorId)
  ├── cmd: MemCmd            // ReadReq, WriteReq, ReadResp, WriteResp, ...
  ├── addr: Addr             // physical address
  ├── size: unsigned         // transfer size in bytes
  └── data: uint8_t*         // payload data (dynamically allocated)
```

The same `Packet` object flows from requester → responder → back to requester (command flipped to response).
Ownership: the requester allocates the packet, the final receiver deletes it.

### 2.5 ClockedObject

[`BaseTrafficGen`](../../src/cpu/testers/traffic_gen/base.hh#L67) inherits from [`ClockedObject`](../../src/sim/clocked_object.hh#L234), which is [`SimObject`](../../src/sim/sim_object.hh#L146) + clock awareness.
Key methods: `clockPeriod()`, `clockEdge()`, `curCycle()`.
The traffic generator does not really use clock edges — it works in **tick-granularity** via inter-packet periods.
But it inherits `ClockedObject` because the gem5 board infrastructure expects processor-like components to have a clock domain.

### 2.6 RequestorID — Who Sent This Packet?

Each traffic source registers with the `System` to get a unique 16-bit `RequestorID`:

```cpp
requestorId = system->getRequestorId(this);
```

This ID is stamped into every [`Request`](../../src/mem/request.hh#L97) the generator creates.
Caches and memory controllers use it for per-source statistics (e.g., "how many cache misses did generator #2 cause?").

---

## 3. Architecture Overview

The LinearGenerator spans **four layers**, from user-facing Python down to C++ packet generation:

```
┌─────────────────────────────────────────────────────────────────────┐
│  User config script  (e.g., memory_traffic.py)                      │
│    generator = LinearGenerator(num_cores=2, rate="40GiB/s", ...)    │
└────────────────────────────┬────────────────────────────────────────┘
                             │ creates
┌────────────────────────────▼────────────────────────────────────────┐
│  Layer 1: Python stdlib wrappers                                    │
│    LinearGenerator  →  LinearGeneratorCore  →  AbstractGenerator    │
│    (address partitioning, parameter bundling)                       │
└────────────────────────────┬────────────────────────────────────────┘
                             │ wraps
┌────────────────────────────▼────────────────────────────────────────┐
│  Layer 2: SimObject Python class + pybind11 bridge                  │
│    PyTrafficGen.py  →  PyTrafficGen (C++)                           │
│    (Python iterator → C++ generator handoff)                        │
└────────────────────────────┬────────────────────────────────────────┘
                             │ inherits
┌────────────────────────────▼────────────────────────────────────────┐
│  Layer 3: C++ simulation engine                                     │
│    BaseTrafficGen (event loop, port, retry, stats)                  │
└────────────────────────────┬────────────────────────────────────────┘
                             │ delegates to
┌────────────────────────────▼────────────────────────────────────────┐
│  Layer 4: C++ packet factory                                        │
│    LinearGen : StochasticGen : BaseGen                               │
│    (sequential addr walk, read/write decision, packet construction) │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 4. Layer 1 — Python Configuration Layer

Three classes collaborate at this layer, each with a distinct responsibility:

| Class | Responsibility |
|-------|---------------|
| **`LinearGenerator`** | User-facing API. Takes high-level parameters (`num_cores`, `rate`, `duration`), partitions the address space, and creates one core per partition. Plugs into the board as a processor replacement. |
| **`LinearGeneratorCore`** | Per-core wrapper. Owns one `PyTrafficGen` C++ SimObject, converts human-readable parameters (e.g. `"40GiB/s"`) into tick-level values, and defines the traffic sequence via a Python generator function. |
| **`PyTrafficGen`** | The actual C++ simulation engine (a SimObject). Owns the `RequestPort`, runs the event loop, sends packets, handles retries, and collects statistics. It knows nothing about "linear" vs. "random" — it just consumes an iterator of `BaseGen` pattern objects. |

**Why three classes instead of one?**
gem5's board/processor framework expects a processor to contain multiple cores, each with its own cache port.
`LinearGenerator` satisfies that contract: it *is* a processor (inherits `AbstractProcessor`) and holds a list of cores.
Each `LinearGeneratorCore` *is* a core (inherits `AbstractCore`) and connects to one cache port.
But the actual packet-generation machinery lives in `PyTrafficGen`, a general-purpose C++ SimObject that is reused by *all* generator types (random, strided, DRAM, trace, etc.).
The Python wrapper classes exist to translate user intent ("40 GiB/s linear sweep") into the low-level C++ parameterization (`min_period=1490 ticks, max_period=1490 ticks`), and to plug into the board framework that expects processor-shaped objects.

In short: `LinearGenerator` **configures**, `LinearGeneratorCore` **translates**, `PyTrafficGen` **executes**.

### 4.1 LinearGenerator

**File:** [src/python/gem5/components/processors/linear_generator.py](../../src/python/gem5/components/processors/linear_generator.py)

This is the user-facing class. It inherits from [`AbstractGenerator`](../../src/python/gem5/components/processors/abstract_generator.py#L53) (which inherits from [`AbstractProcessor`](../../src/python/gem5/components/processors/abstract_processor.py)).
The key insight: **a `LinearGenerator` replaces a CPU in the board topology.**
Where you would normally plug in a `SimpleProcessor` with real CPU cores, you instead plug in a `LinearGenerator` with synthetic traffic cores.

```python
class LinearGenerator(AbstractGenerator):
    def __init__(
        self,
        num_cores: int = 1,       # Number of independent traffic sources
        duration: str = "1ms",    # How long to generate traffic
        rate: str = "100GiB/s",   # Target injection bandwidth
        block_size: int = 64,     # Bytes per request (typically = cache line)
        min_addr: int = 0,        # Start of address range
        max_addr: int = 32768,    # End of address range
        rd_perc: int = 100,       # 100 = all reads, 0 = all writes
        data_limit: int = 0,      # Stop after N bytes (0 = no limit)
    ) -> None:
```

**What [`_create_cores`](../../src/python/gem5/components/processors/linear_generator.py#L85) does:**

```python
def _create_cores(self, num_cores, ...):
    ranges = partition_range(min_addr, max_addr, num_cores)
    return [
        LinearGeneratorCore(
            min_addr=ranges[i][0],
            max_addr=ranges[i][1],
            ...
        )
        for i in range(num_cores)
    ]
```

When you have `num_cores=4` and `max_addr=32768`, each core gets a disjoint 8192-byte slice:
- Core 0: [0, 8192)
- Core 1: [8192, 16384)
- Core 2: [16384, 24576)
- Core 3: [24576, 32768)

This partitioning prevents address conflicts between cores and ensures each core exercises a unique portion of the address space.

> **Note:** `partition_range` requires `(max_addr - min_addr) % num_cores == 0`. It will assert-fail otherwise.

### 4.2 LinearGeneratorCore

**File:** [src/python/gem5/components/processors/linear_generator_core.py](../../src/python/gem5/components/processors/linear_generator_core.py)

Each core wraps **one** [`PyTrafficGen`](../../src/cpu/testers/traffic_gen/pygen.hh#L52) SimObject — the actual C++ traffic engine:

```python
class LinearGeneratorCore(AbstractGeneratorCore):
    def __init__(self, duration, rate, block_size, min_addr, max_addr, rd_perc, data_limit):
        super().__init__()
        self.generator = PyTrafficGen()      # ← The C++ SimObject
        self._duration = duration
        self._rate = rate
        # ... store all params
```

**Port connection — [`connect_dcache`](../../src/python/gem5/components/processors/linear_generator_core.py#L90):**

```python
def connect_dcache(self, port: Port) -> None:
    self.generator.port = port    # Wire PyTrafficGen's RequestPort to the cache
```

When the board calls `connect_dcache`, the generator's request port gets bound to whatever is on the other side (typically an L1 cache's CPU-side response port).

**The Python generator pattern — [`_create_traffic`](../../src/python/gem5/components/processors/linear_generator_core.py#L99) (critical to understand):**

```python
def _create_traffic(self) -> Iterator[BaseTrafficGen]:
    duration = fromSeconds(toLatency(self._duration))     # "1ms" → ticks
    rate = toMemoryBandwidth(self._rate)                  # "100GiB/s" → bytes/sec
    period = fromSeconds(self._block_size / rate)          # ticks between packets
    min_period = period
    max_period = period

    yield self.generator.createLinear(
        duration,
        self._min_addr, self._max_addr,
        self._block_size,
        min_period, max_period,
        self._rd_perc,
        self._data_limit,
    )
    yield self.generator.createExit(0)
```

This is a **Python generator function** (uses `yield`).
It produces exactly **two** items:
1. A [`LinearGen`](../../src/cpu/testers/traffic_gen/linear_gen.hh#L62) C++ object (the sequential traffic pattern)
2. An [`ExitGen`](../../src/cpu/testers/traffic_gen/exit_gen.hh) C++ object (tells the simulator to stop)

The `createLinear()` call invokes a C++ factory method on the `PyTrafficGen` SimObject (exposed via pybind11).
It returns a `shared_ptr<BaseGen>` — the C++ traffic pattern object.

**Rate-to-period conversion:**

```
rate = "100GiB/s"  →  107,374,182,400 bytes/sec
block_size = 64 bytes
period = 64 / 107,374,182,400 sec ≈ 5.96e-10 sec
In ticks (1 tick = 1 ps): period ≈ 596 ticks
```

This means: "issue one 64-byte request every ~596 picoseconds."

**Starting traffic — [`start_traffic`](../../src/python/gem5/components/processors/linear_generator_core.py#L125):**

```python
def start_traffic(self) -> None:
    self._set_traffic()                    # Creates the Python iterator
    self.generator.start(self._traffic)    # Passes iterator to C++
```

### 4.3 AbstractGeneratorCore

**File:** [src/python/gem5/components/processors/abstract_generator_core.py](../../src/python/gem5/components/processors/abstract_generator_core.py)

This base class makes generator cores look like CPU cores to the rest of the framework:

- `get_isa()` → returns `ISA.NULL` (not a real CPU)
- `connect_icache()` → routes to a `PortTerminator` (generators don't fetch instructions)
- `connect_walker_ports()` → routes to `PortTerminator` (no page table walks)
- `set_workload()` → no-op (no binary to run)
- `connect_interrupt()` → no-op

### 4.4 AbstractGenerator and [`_post_instantiate`](../../src/python/gem5/components/processors/abstract_generator.py#L85)

**File:** [src/python/gem5/components/processors/abstract_generator.py](../../src/python/gem5/components/processors/abstract_generator.py)

```python
class AbstractGenerator(AbstractProcessor):
    def incorporate_processor(self, board):
        board.set_mem_mode(MemMode.TIMING)    # Generators require timing mode

    def _post_instantiate(self):
        self.start_traffic()    # Auto-start traffic after m5.instantiate()
```

The `_post_instantiate` hook is called by the gem5 stdlib framework **after** `m5.instantiate()` has created all C++ objects and bound all ports.
This is when `start_traffic()` fires, which cascades down to each core's `self.generator.start(self._traffic)`.

---

## 5. Layer 2 — The Python-to-C++ Bridge (PyTrafficGen)

### 5.1 The SimObject Definition

**File:** [src/cpu/testers/traffic_gen/PyTrafficGen.py](../../src/cpu/testers/traffic_gen/PyTrafficGen.py)

```python
class PyTrafficGen(BaseTrafficGen):
    type = "PyTrafficGen"
    cxx_header = "cpu/testers/traffic_gen/pygen.hh"
    cxx_class = "gem5::PyTrafficGen"

    @cxxMethod
    def start(self, meta_generator):
        pass

    cxx_exports = [
        PyBindMethod("createIdle"),
        PyBindMethod("createExit"),
        PyBindMethod("createLinear"),
        PyBindMethod("createRandom"),
        # ...
    ]
```

**How this works:**

1. `type = "PyTrafficGen"` registers this as a SimObject type in gem5's object system.
2. `cxx_class` and `cxx_header` tell the build system which C++ class to instantiate.
3. `@cxxMethod` marks `start()` as a method implemented in C++ and callable from Python.
4. `cxx_exports` exposes C++ factory methods (`createLinear`, etc.) to Python via pybind11.

When you write `self.generator.createLinear(...)` in Python, you're calling `BaseTrafficGen::createLinear()` in C++.
The return value (a `shared_ptr<BaseGen>`) is automatically wrapped by pybind11 and returned as a Python object.

### 5.2 The C++ Side

**File:** [src/cpu/testers/traffic_gen/pygen.hh](../../src/cpu/testers/traffic_gen/pygen.hh)

```cpp
class PyTrafficGen : public BaseTrafficGen
{
  public:
    PyTrafficGen(const PyTrafficGenParams &p);

  public: // Python API
    void start(pybind11::object meta_generator);

  protected:
    std::shared_ptr<BaseGen> nextGenerator() override;

  protected:
    pybind11::iterator metaGenerator;
};
```

**File:** [src/cpu/testers/traffic_gen/pygen.cc](../../src/cpu/testers/traffic_gen/pygen.cc)

```cpp
void
PyTrafficGen::start(pybind11::object meta_generator)
{
    metaGenerator = meta_generator.begin();  // Convert Python iterable → iterator
    BaseTrafficGen::start();                 // Kick off the event loop
}

std::shared_ptr<BaseGen>
PyTrafficGen::nextGenerator()
{
    if (!metaGenerator)
        return std::shared_ptr<BaseGen>();               // No iterator

    if (metaGenerator == py::iterator::sentinel())
        return std::shared_ptr<BaseGen>();               // Iterator exhausted

    std::shared_ptr<BaseGen> gen =
        metaGenerator->cast<std::shared_ptr<BaseGen>>(); // Cast Python obj → C++
    metaGenerator++;                                     // Advance iterator
    return gen;
}
```

This is the bridge:
- Python's `_create_traffic()` generator `yield`s C++ [`BaseGen`](../../src/cpu/testers/traffic_gen/base_gen.hh#L65) objects.
- `nextGenerator()` pulls the next one via the pybind11 iterator protocol.
- First call returns the `LinearGen`, second call returns the `ExitGen`, third call hits the sentinel.

The pybind11 registration at the bottom of `pygen.cc` makes `BaseGen` known to Python:
```cpp
void pybind_init_tracers(py::module_ &m_native) {
    py::module_ m = m_native.def_submodule("trace");
    py::class_<BaseGen, std::shared_ptr<BaseGen>> c_base(m, "BaseGen");
}
static EmbeddedPyBind _py_tracers("trace", pybind_init_tracers);
```

---

## 6. Layer 3 — The C++ Simulation Engine (BaseTrafficGen)

**File:** [src/cpu/testers/traffic_gen/base.hh](../../src/cpu/testers/traffic_gen/base.hh) and [base.cc](../../src/cpu/testers/traffic_gen/base.cc)

This is the heart of the traffic generator — the event-driven engine that drives packet generation.

### 6.1 Class Structure

```cpp
class BaseTrafficGen : public ClockedObject
{
  protected:
    System *const system;            // For address validation and RequestorID
    const bool elasticReq;           // Respond to back-pressure?
    const Tick progressCheck;        // Watchdog timeout

  private:
    EventFunctionWrapper noProgressEvent;  // Watchdog: fatal if stuck
    EventFunctionWrapper updateEvent;      // Main driver: calls update()

    Tick nextTransitionTick;   // When to switch to next generator
    Tick nextPacketTick;       // When to send next packet

    TrafficGenPort port;       // RequestPort to memory hierarchy
    PacketPtr retryPkt;        // Packet waiting for retry
    Tick retryPktTick;         // When retryPkt was originally due
    bool blockedWaitingResp;   // Blocked on max outstanding reqs

    const int maxOutstandingReqs;

  protected:
    std::unordered_map<RequestPtr, Tick> waitingResp;  // In-flight requests
    const RequestorID requestorId;
    std::shared_ptr<BaseGen> activeGenerator;           // Current pattern
};
```

### 6.2 The [`TrafficGenPort`](../../src/cpu/testers/traffic_gen/base.hh#L130) Inner Class

```cpp
class TrafficGenPort : public RequestPort
{
    void recvReqRetry()          { trafficGen.recvReqRetry(); }
    bool recvTimingResp(PacketPtr pkt) { return trafficGen.recvTimingResp(pkt); }
    void recvTimingSnoopReq(PacketPtr pkt) { }  // Ignore snoops
};
```

This is a thin adapter: it receives port callbacks and delegates to `BaseTrafficGen` methods.
Snoops are no-ops because the generator doesn't cache data — it just injects requests.

### 6.3 The [`start()`](../../src/cpu/testers/traffic_gen/base.cc#L282) Method — Kicking Off Simulation

```cpp
void BaseTrafficGen::start()
{
    transition();       // Get the first generator (LinearGen)
    scheduleUpdate();   // Schedule the first updateEvent
}
```

Called by `PyTrafficGen::start()` after the Python iterator is stored.

### 6.4 The [`transition()`](../../src/cpu/testers/traffic_gen/base.cc#L230) Method — Switching Generators

```cpp
void BaseTrafficGen::transition()
{
    if (activeGenerator)
        activeGenerator->exit();        // Clean up previous generator

    activeGenerator = nextGenerator();  // Pull next from Python iterator

    if (activeGenerator) {
        const Tick duration = activeGenerator->duration;
        if (duration != MaxTick && duration != 0)
            nextTransitionTick = curTick() + duration;  // "1ms from now"
        else
            nextTransitionTick = MaxTick;               // Run forever

        activeGenerator->enter();       // Initialize generator state
        nextPacketTick = activeGenerator->nextPacketTick(elasticReq, 0);
    } else {
        // No more generators — stop
        nextPacketTick = MaxTick;
        nextTransitionTick = MaxTick;
    }
}
```

For the LinearGenerator flow:
1. First `transition()`: pulls `LinearGen` → sets `nextTransitionTick = curTick() + duration`
2. After duration expires, second `transition()`: pulls `ExitGen` → triggers simulation exit
3. Third `transition()`: iterator exhausted → null → simulation stops

### 6.5 The [`update()`](../../src/cpu/testers/traffic_gen/base.cc#L169) Method — The Main Event Loop

This is the most important method. It runs every time `updateEvent` fires:

```cpp
void BaseTrafficGen::update()
{
    // 1. Reset watchdog
    reschedule(noProgressEvent, curTick() + progressCheck, true);

    // 2. Check for generator transition
    if (curTick() >= nextTransitionTick) {
        transition();
    } else {
        // 3. Generate a packet
        PacketPtr pkt = activeGenerator->getNextPacket();

        // 4. (Optional) Assign stream IDs
        if (streamGenerator) { /* ... */ }

        // 5. Validate and send
        if (pkt && system->isMemAddr(pkt->getAddr())) {
            stats.numPackets++;
            blockedWaitingResp = allocateWaitingRespSlot(pkt);

            if (blockedWaitingResp || !port.sendTimingReq(pkt)) {
                retryPkt = pkt;         // Stash for retry
                retryPktTick = curTick();
            }
        } else if (pkt) {
            // Address not in memory range — suppress and delete
            ++stats.numSuppressed;
            delete pkt;
        }
    }

    // 6. Schedule next event (only if not waiting for retry)
    if (retryPkt == NULL) {
        nextPacketTick = activeGenerator->nextPacketTick(elasticReq, 0);
        scheduleUpdate();
    }
}
```

**Step-by-step execution semantics:**

| Step | What happens | What can go wrong |
|------|-------------|-------------------|
| 1 | Watchdog pushed forward by `progressCheck` ticks | If `update()` never runs again, watchdog fires → `fatal()` |
| 2 | If duration expired, switch to next generator (ExitGen) | — |
| 3 | `LinearGen::getNextPacket()` creates a Packet | — |
| 5a | `allocateWaitingRespSlot()` tracks in-flight requests | Returns `true` if `maxOutstandingReqs` exceeded → blocks |
| 5b | `port.sendTimingReq()` pushes packet into memory hierarchy | Returns `false` if peer is busy → must retry |
| 6 | Ask generator for next packet time, schedule `updateEvent` | If `retryPkt` is set, we don't schedule — wait for retry callback |

### 6.6 The [`scheduleUpdate()`](../../src/cpu/testers/traffic_gen/base.cc#L258) Method

```cpp
void BaseTrafficGen::scheduleUpdate()
{
    // Force transition if generator ran out of work
    while (activeGenerator &&
           nextPacketTick == MaxTick && nextTransitionTick == MaxTick) {
        transition();
    }

    if (!activeGenerator)
        return;  // All generators exhausted

    const Tick nextEventTick = std::min(nextPacketTick, nextTransitionTick);
    schedule(updateEvent, std::max(curTick(), nextEventTick));
}
```

The `std::max(curTick(), nextEventTick)` prevents scheduling events in the past (can happen after a retry delay).

### 6.7 The [`recvTimingResp()`](../../src/cpu/testers/traffic_gen/base.cc#L559) Method — Handling Responses

```cpp
bool BaseTrafficGen::recvTimingResp(PacketPtr pkt)
{
    auto iter = waitingResp.find(pkt->req);
    // ... panic if not found

    // Calculate latency
    if (pkt->isWrite()) {
        ++stats.totalWrites;
        stats.bytesWritten += pkt->req->getSize();
        stats.totalWriteLatency += curTick() - iter->second;
    } else {
        ++stats.totalReads;
        stats.bytesRead += pkt->req->getSize();
        stats.totalReadLatency += curTick() - iter->second;
    }

    waitingResp.erase(iter);
    delete pkt;

    // If we were blocked on max outstanding, try to send again
    if (blockedWaitingResp) {
        blockedWaitingResp = false;
        retryReq();
    }
    return true;
}
```

The `waitingResp` map stores `{RequestPtr → send_tick}`.
When the response arrives, `curTick() - send_tick` gives the **round-trip latency** for that request.

---

## 7. Layer 4 — The LinearGen Packet Factory

### 7.1 Class Hierarchy

```
BaseGen                    (abstract: enter/exit/getNextPacket/nextPacketTick)
  └── StochasticGen        (adds: startAddr, endAddr, blocksize, period, readPercent)
        └── LinearGen      (adds: nextAddr, dataManipulated — sequential walk)
```

### 7.2 BaseGen — The Foundation

**File:** [src/cpu/testers/traffic_gen/base_gen.hh](../../src/cpu/testers/traffic_gen/base_gen.hh)

```cpp
class BaseGen
{
  protected:
    const std::string _name;
    const RequestorID requestorId;
    mutable Random::RandomPtr rng;

    PacketPtr getPacket(Addr addr, unsigned size, const MemCmd& cmd,
                        Request::FlagsType flags = 0);

  public:
    const Tick duration;

    virtual void enter() = 0;
    virtual PacketPtr getNextPacket() = 0;
    virtual void exit() { }
    virtual Tick nextPacketTick(bool elastic, Tick delay) const = 0;
};
```

**The [`getPacket()`](../../src/cpu/testers/traffic_gen/base_gen.cc#L55) factory method** (from `base_gen.cc`):

```cpp
PacketPtr BaseGen::getPacket(Addr addr, unsigned size, const MemCmd& cmd,
                              Request::FlagsType flags)
{
    // 1. Create Request (address, size, flags, requestorId)
    RequestPtr req = std::make_shared<Request>(addr, size, flags, requestorId);

    // 2. Set dummy PC (gives prefetchers something to latch onto)
    req->setPC(((Addr)requestorId) << 2);

    // 3. Wrap in Packet
    PacketPtr pkt = new Packet(req, cmd);

    // 4. Allocate and attach data buffer
    uint8_t* pkt_data = new uint8_t[req->getSize()];
    pkt->dataDynamic(pkt_data);  // Packet takes ownership (deletes on destruction)

    // 5. For writes, fill with a pattern (requestorId byte repeated)
    if (cmd.isWrite()) {
        std::fill_n(pkt_data, req->getSize(), (uint8_t)requestorId);
    }

    return pkt;
}
```

### 7.3 StochasticGen — Shared Parameters

**File:** [src/cpu/testers/traffic_gen/base_gen.hh](../../src/cpu/testers/traffic_gen/base_gen.hh)

```cpp
class StochasticGen : public BaseGen
{
  protected:
    const Addr startAddr;      // First address in range
    const Addr endAddr;        // One past last address
    const Addr blocksize;      // Bytes per request
    const Addr cacheLineSize;  // System cache line size (for validation)
    const Tick minPeriod;      // Minimum ticks between packets
    const Tick maxPeriod;      // Maximum ticks between packets
    const uint8_t readPercent; // 0-100
    const Addr dataLimit;      // 0 = unlimited
};
```

Constructor validation (from `base_gen.cc`):
- `blocksize > cacheLineSize` → **fatal** (a single request cannot span cache lines)
- `readPercent > 100` → **fatal**
- `minPeriod > maxPeriod` → **fatal**

### 7.4 LinearGen — The Sequential Walker

**File:** [src/cpu/testers/traffic_gen/linear_gen.hh](../../src/cpu/testers/traffic_gen/linear_gen.hh) and [linear_gen.cc](../../src/cpu/testers/traffic_gen/linear_gen.cc)

```cpp
class LinearGen : public StochasticGen
{
  private:
    Addr nextAddr;          // Current position in the address walk
    Addr dataManipulated;   // Running total of bytes issued
};
```

**[`enter()`](../../src/cpu/testers/traffic_gen/linear_gen.cc#L49)** — Called when this generator becomes active:

```cpp
void LinearGen::enter()
{
    nextAddr = startAddr;     // Reset to beginning of range
    dataManipulated = 0;      // Reset byte counter
}
```

**[`getNextPacket()`](../../src/cpu/testers/traffic_gen/linear_gen.cc#L57)** — Creates the next sequential request:

```cpp
PacketPtr LinearGen::getNextPacket()
{
    // 1. Decide read vs. write
    bool isRead = readPercent != 0 &&
        (readPercent == 100 || rng->random(0, 100) < readPercent);

    // 2. Count bytes
    dataManipulated += blocksize;

    // 3. Build packet at current address
    PacketPtr pkt = getPacket(nextAddr, blocksize,
                              isRead ? MemCmd::ReadReq : MemCmd::WriteReq);

    // 4. Advance address linearly
    nextAddr += blocksize;

    // 5. Wrap around at end of range
    if (nextAddr >= endAddr)
        nextAddr = startAddr;

    return pkt;
}
```

**Address walk visualization** (blocksize=64, range=[0x0, 0x200)):

```
Call 1: addr=0x000  nextAddr→0x040
Call 2: addr=0x040  nextAddr→0x080
Call 3: addr=0x080  nextAddr→0x0C0
...
Call 8: addr=0x1C0  nextAddr→0x200 → wraps → nextAddr=0x000
Call 9: addr=0x000  (cycle repeats)
```

**[`nextPacketTick()`](../../src/cpu/testers/traffic_gen/linear_gen.cc#L90)** — When should the next packet be sent?

```cpp
Tick LinearGen::nextPacketTick(bool elastic, Tick delay) const
{
    // Check data limit
    if (dataLimit && dataManipulated >= dataLimit)
        return MaxTick;   // Done — no more packets

    // Random wait in [minPeriod, maxPeriod]
    Tick wait = rng->random(minPeriod, maxPeriod);

    // Compensate for delay (non-elastic mode)
    if (!elastic) {
        if (wait < delay)
            wait = 0;
        else
            wait -= delay;
    }

    return curTick() + wait;
}
```

When `minPeriod == maxPeriod` (which is the case for LinearGenerator — see Section 4.2), the inter-packet time is deterministic.

---

## 8. End-to-End Packet Lifecycle

Here is the complete journey of a single packet, from Python config to memory response:

```
   Python script                     C++ Simulation Engine
  ────────────────                  ──────────────────────

  1. LinearGenerator(rate="40GiB/s", duration="1ms", ...)
       │
  2. _create_cores() → LinearGeneratorCore(min_addr=0, max_addr=8192, ...)
       │
  3. LinearGeneratorCore.__init__()
       │  self.generator = PyTrafficGen()
       │
  4. Board wires ports: generator.port ↔ cache.cpu_side
       │
  5. m5.instantiate() → C++ objects created, ports bound
       │
  6. _post_instantiate() → start_traffic()
       │
  7. _set_traffic() → _create_traffic() → Python generator object
       │
  8. generator.start(self._traffic)  ──────→  PyTrafficGen::start()
       │                                         │
       │                                    9. metaGenerator = meta_generator.begin()
       │                                    10. BaseTrafficGen::start()
       │                                         │
       │                                    11. transition()
       │                                         │  nextGenerator() → pulls LinearGen
       │                                         │  activeGenerator = LinearGen
       │                                         │  activeGenerator->enter()
       │                                         │    → nextAddr=startAddr, dataManipulated=0
       │                                         │  nextTransitionTick = curTick() + duration
       │                                         │
       │                                    12. scheduleUpdate()
       │                                         │  schedule(updateEvent, nextPacketTick)
       │                                         │
       │                                    ═══════════════════════════════
       │                                    ║  Event loop fires updateEvent ║
       │                                    ═══════════════════════════════
       │                                         │
       │                                    13. update()
       │                                         │  pkt = LinearGen::getNextPacket()
       │                                         │    → addr=0x0, size=64, ReadReq
       │                                         │
       │                                    14. port.sendTimingReq(pkt)
       │                                         │
       │                                         ▼
       │                                    ┌─────────────┐
       │                                    │   L1 Cache   │
       │                                    │  (miss)      │
       │                                    └──────┬──────┘
       │                                           │
       │                                           ▼
       │                                    ┌─────────────┐
       │                                    │   L2 Cache   │
       │                                    └──────┬──────┘
       │                                           │
       │                                           ▼
       │                                    ┌─────────────┐
       │                                    │  Mem Ctrl    │
       │                                    │  (DRAM)      │
       │                                    └──────┬──────┘
       │                                           │
       │                                    15. Response propagates back up
       │                                           │
       │                                    16. recvTimingResp(pkt)
       │                                         │  latency = curTick() - sendTick
       │                                         │  update stats
       │                                         │  delete pkt
       │                                         │
       │                                    17. (If blocked) → retryReq() → scheduleUpdate()
       │                                         │
       │                                    [Back to step 13 — next updateEvent]
```

---

## 9. Timing Model and Rate Control

The generator does **not** model a pipeline or microarchitecture.
Its timing is controlled by a single parameter: the **inter-packet period**.

### 9.1 How Period Is Calculated

In `LinearGeneratorCore._create_traffic()`:

```python
rate = toMemoryBandwidth(self._rate)             # "40GiB/s" → bytes/second
period = fromSeconds(self._block_size / rate)     # seconds → ticks
```

For `rate="40GiB/s"` and `block_size=64`:
```
rate = 40 × 2^30 = 42,949,672,960 bytes/sec
period = 64 / 42,949,672,960 ≈ 1.49e-9 sec = 1490 ps = 1490 ticks
```

Both `min_period` and `max_period` are set to the same value, so the inter-packet time is **deterministic** (no randomness).

### 9.2 Elastic vs. Inelastic Mode

The `elastic_req` parameter (default: `False`) controls how the generator responds to back-pressure:

**Inelastic (default):** The generator tries to maintain the target rate.
If a packet is delayed by `delay` ticks due to a retry, the *next* packet's wait is reduced:

```cpp
if (!elastic) {
    if (wait < delay)
        wait = 0;         // Can't go negative — send immediately
    else
        wait -= delay;    // Compensate for lost time
}
```

This means: after a retry delay, the generator "catches up" by sending the next packet sooner.

**Elastic:** The generator absorbs back-pressure.
`wait` is not adjusted, so the effective rate slows down when the memory system pushes back.

### 9.3 Duration and Data Limit

Two orthogonal stop conditions:

1. **Duration:** After `nextTransitionTick` ticks, `update()` calls `transition()`, which pulls the `ExitGen` from the Python iterator.
2. **Data limit:** If `dataManipulated >= dataLimit`, `nextPacketTick()` returns `MaxTick`, halting packet generation within the current generator.

If `data_limit=0` (default), only the duration matters.
If both are set, whichever triggers first wins.

---

## 10. Back-Pressure and Retry Protocol

This is one of the trickiest aspects of gem5's timing mode.

### 10.1 The Handshake

```
Generator                    Cache (or other ResponsePort peer)
─────────                    ──────────────────────────────────
port.sendTimingReq(pkt)  →   recvTimingReq(pkt)
                              │
                              ├─ returns true  → packet accepted, generator continues
                              │
                              └─ returns false → packet rejected (busy/full)
                                                 Generator must:
                                                   1. Save pkt as retryPkt
                                                   2. Stop scheduling updateEvent
                                                   3. Wait for...
                              ...later...
                              sendRetryReq()  →   recvReqRetry()
                                                   Generator calls retryReq():
                                                     port.sendTimingReq(retryPkt)
                                                     → if accepted: resume normal operation
                                                     → if rejected again: wait again
```

### 10.2 Outstanding Request Limiting

If `maxOutstandingReqs > 0`, the generator tracks in-flight requests in `waitingResp`:

```cpp
bool allocateWaitingRespSlot(PacketPtr pkt) {
    waitingResp[pkt->req] = curTick();
    return (maxOutstandingReqs > 0) &&
           (waitingResp.size() > maxOutstandingReqs);
}
```

If the map exceeds the limit, `blockedWaitingResp` is set and the packet is stashed.
When a response arrives (`recvTimingResp`), the map shrinks and `retryReq()` is called to unblock.

### 10.3 The Watchdog

```cpp
EventFunctionWrapper noProgressEvent([this]{ noProgress(); }, name());
```

Every `update()` call reschedules the watchdog to `curTick() + progressCheck`.
If `progressCheck` ticks pass without any `update()` call (e.g., stuck in retry with no retry callback), the watchdog fires:

```cpp
void BaseTrafficGen::noProgress() {
    fatal("BaseTrafficGen %s spent %llu ticks without making progress",
          name(), progressCheck);
}
```

Default `progressCheck` is "1ms" (1,000,000 ticks at 1ps resolution).

---

## 11. Statistics Collection

After simulation, gem5 dumps statistics to `stats.txt` in the output directory.
The traffic generator reports:

| Statistic | Meaning |
|-----------|---------|
| `numPackets` | Total packets generated |
| `numSuppressed` | Packets dropped (address not in memory range) |
| `numRetries` | Times the port rejected a request |
| `retryTicks` | Total ticks lost waiting for retries |
| `bytesRead` / `bytesWritten` | Data volume |
| `totalReads` / `totalWrites` | Request counts |
| `totalReadLatency` / `totalWriteLatency` | Sum of per-request round-trip latencies |
| `avgReadLatency` / `avgWriteLatency` | `totalLatency / count` |
| `readBW` / `writeBW` | `bytesRead / simSeconds` |

These are defined in `BaseTrafficGen::StatGroup` using gem5's statistics framework.

---

## 12. Complete File Map

### Python Configuration Layer

| File | Role |
|------|------|
| [linear_generator.py](../../src/python/gem5/components/processors/linear_generator.py) | User-facing `LinearGenerator` class |
| [linear_generator_core.py](../../src/python/gem5/components/processors/linear_generator_core.py) | Per-core wrapper, rate→period conversion, Python generator |
| [abstract_generator.py](../../src/python/gem5/components/processors/abstract_generator.py) | Base class, address partitioning, `_post_instantiate` |
| [abstract_generator_core.py](../../src/python/gem5/components/processors/abstract_generator_core.py) | Stubs for icache/walker/interrupt (not used by generators) |
| [abstract_processor.py](../../src/python/gem5/components/processors/abstract_processor.py) | Base processor interface |
| [abstract_core.py](../../src/python/gem5/components/processors/abstract_core.py) | Base core interface |

### SimObject Definitions (Python → C++ bridge)

| File | Role |
|------|------|
| [PyTrafficGen.py](../../src/cpu/testers/traffic_gen/PyTrafficGen.py) | SimObject definition, exports C++ methods to Python |
| [BaseTrafficGen.py](../../src/cpu/testers/traffic_gen/BaseTrafficGen.py) | Base SimObject params (port, elastic_req, progress_check, ...) |

### C++ Implementation

| File | Role |
|------|------|
| [pygen.hh](../../src/cpu/testers/traffic_gen/pygen.hh) / [pygen.cc](../../src/cpu/testers/traffic_gen/pygen.cc) | `PyTrafficGen`: Python iterator → C++ generator bridge |
| [base.hh](../../src/cpu/testers/traffic_gen/base.hh) / [base.cc](../../src/cpu/testers/traffic_gen/base.cc) | `BaseTrafficGen`: event loop, port, retry, stats |
| [base_gen.hh](../../src/cpu/testers/traffic_gen/base_gen.hh) / [base_gen.cc](../../src/cpu/testers/traffic_gen/base_gen.cc) | `BaseGen`: abstract generator + `StochasticGen` + packet factory |
| [linear_gen.hh](../../src/cpu/testers/traffic_gen/linear_gen.hh) / [linear_gen.cc](../../src/cpu/testers/traffic_gen/linear_gen.cc) | `LinearGen`: sequential address walk |
| [exit_gen.hh](../../src/cpu/testers/traffic_gen/exit_gen.hh) / [exit_gen.cc](../../src/cpu/testers/traffic_gen/exit_gen.cc) | `ExitGen`: triggers `m5_exit()` |
| [idle_gen.hh](../../src/cpu/testers/traffic_gen/idle_gen.hh) | `IdleGen`: does nothing for N ticks |

---

## 13. Worked Example with Numerical Values

**Configuration:**

```python
generator = LinearGenerator(
    num_cores=2,
    duration="250us",
    rate="40GiB/s",
    block_size=64,
    min_addr=0,
    max_addr=65536,
    rd_perc=70,
    data_limit=0,
)
```

**Step 1: Address partitioning**

```
Core 0: [0x0000, 0x8000)     # 0 to 32768
Core 1: [0x8000, 0x10000)    # 32768 to 65536
```

**Step 2: Rate → Period**

```
rate = 40 × 2^30 = 42,949,672,960 bytes/sec
period = 64 / 42,949,672,960 sec
       = 1.4901e-9 sec
       = 1,490.116 ps
       ≈ 1,490 ticks (at 1 tick = 1 ps)
```

Each core issues one 64-byte request every ~1,490 ticks.

**Step 3: Duration in ticks**

```
duration = 250 µs = 250,000,000 ps = 250,000,000 ticks
```

**Step 4: Total requests per core**

```
requests = duration / period = 250,000,000 / 1,490 ≈ 167,785 requests
total bytes = 167,785 × 64 ≈ 10.7 MB per core
```

**Step 5: Address wraps per core**

```
range_size = 32,768 bytes
requests_per_sweep = 32,768 / 64 = 512
total_sweeps = 167,785 / 512 ≈ 328 full sweeps of the range
```

**Step 6: Read/Write mix**

With `rd_perc=70`, approximately:
- ~117,450 reads (~7.5 MB read)
- ~50,335 writes (~3.2 MB written)

---

## 14. Common Pitfalls and Debugging

### 14.1 "The port is not connected!"

```
fatal: The port of system.processor.cores0.generator is not connected!
```

This means the board didn't wire the generator's port to a cache or memory.
Check that `connect_dcache()` is being called by your board's `_connect_components()`.

### 14.2 "No progress" Fatal

```
fatal: BaseTrafficGen system.processor.cores0.generator spent 1000000 ticks
       without making progress
```

The generator sent a packet, got rejected, and never received a retry callback within `progress_check` time.
Common causes:
- Memory system misconfigured (port not connected to anything functional)
- Cache/controller is stuck in a deadlock

### 14.3 "block size larger than cache line size"

```
fatal: TrafficGen block_size (128) is larger than cache line size (64)
```

[`StochasticGen`](../../src/cpu/testers/traffic_gen/base_gen.hh#L142) requires `blocksize <= cacheLineSize`.
A single request must not span cache lines.

### 14.4 Address Range Not Divisible by `num_cores`

```python
partition_range(0, 1000, 3)  # AssertionError: 1000 % 3 != 0
```

Use an address range where `(max_addr - min_addr) % num_cores == 0`.

### 14.5 Debugging with Trace Flags

Build with the `opt` or `debug` variant and run with:

```bash
./build/RISCV/gem5.opt --debug-flags=TrafficGen \
    -d m5out/debug-tgen-$(date +%Y%m%d-%H%M%S) \
    your_config.py
```

The `TrafficGen` debug flag prints every packet generated:

```
     0: system.tgen: LinearGen::getNextPacket: r to addr 0x0, size 64
  1490: system.tgen: LinearGen::getNextPacket: w to addr 0x40, size 64
  2980: system.tgen: LinearGen::getNextPacket: r to addr 0x80, size 64
```

### 14.6 Memory Mode Must Be Timing

`AbstractGenerator.incorporate_processor()` sets `board.set_mem_mode(MemMode.TIMING)`.
If you're building your own board and forget to call this, or override it to `ATOMIC`, the `sendTimingReq()` calls will fail because the memory system isn't in timing mode.

---

## Appendix A: SystemC ↔ gem5 Quick Reference

| SystemC Concept | gem5 Equivalent | Key Difference |
|----------------|-----------------|----------------|
| `sc_module` | `SimObject` / `ClockedObject` | Config in Python, not C++ constructor |
| `sc_port<IF>` / `sc_export<IF>` | `RequestPort` / `ResponsePort` | Explicit request/response directionality |
| `sc_event` + `notify(delay)` | `EventFunctionWrapper` + `schedule(ev, tick)` | Absolute ticks, not relative delays |
| `sc_time` | `Tick` (uint64_t) | 1 tick = 1 ps by default |
| TLM `generic_payload` | `Packet` + `Request` | Packet is per-hop; Request is persistent |
| `b_transport()` | `sendAtomic()` | Blocking, instantaneous |
| `nb_transport_fw/bw()` | `sendTimingReq()` / `sendTimingResp()` | Non-blocking with retry protocol |
| `SC_METHOD` sensitivity | `schedule(event, tick)` | No automatic re-triggering |
| Module hierarchy | SimObject Python tree | Python builds the tree, C++ executes it |

## Appendix B: Complete Inheritance Chain

**C++ SimObject hierarchy:**

[SimObject](../../src/sim/sim_object.hh#L146) ─► [ClockedObject](../../src/sim/clocked_object.hh#L234) ─► [BaseTrafficGen](../../src/cpu/testers/traffic_gen/base.hh#L67) ─► [PyTrafficGen](../../src/cpu/testers/traffic_gen/pygen.hh#L52)

**Python stdlib wrapper hierarchy:**

[AbstractGeneratorCore](../../src/python/gem5/components/processors/abstract_generator_core.py#L39) ─► [LinearGeneratorCore](../../src/python/gem5/components/processors/linear_generator_core.py#L45)

[AbstractGenerator](../../src/python/gem5/components/processors/abstract_generator.py#L53) ─► [LinearGenerator](../../src/python/gem5/components/processors/linear_generator.py#L37)

**C++ generator pattern hierarchy:**

[BaseGen](../../src/cpu/testers/traffic_gen/base_gen.hh#L65) ─► [StochasticGen](../../src/cpu/testers/traffic_gen/base_gen.hh#L142) ─► [LinearGen](../../src/cpu/testers/traffic_gen/linear_gen.hh#L62)
