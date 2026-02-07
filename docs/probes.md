# gem5 Probe System

The probe system is gem5's **observer pattern** for simulation events. It
allows SimObjects to expose named notification points ("probe points") that
other components can listen to without requiring tight coupling between
producer and consumer.

Probes are the primary mechanism for **non-intrusive instrumentation**:
memory tracing, instruction counting, performance counter modeling (PMUs),
SimPoint profiling, and hardware-prefetcher training all use probes.

## Design Philosophy

The probe infrastructure is guided by four principles:

1. **Zero-cost when unused.** A probe point with no listeners compiles down
   to an empty loop.  The `hasListeners()` predicate lets producers skip
   expensive argument preparation when nobody is listening.

2. **Type safety.** Probe points and listeners are parameterized by a
   single `Arg` type (`ProbePointArg<Arg>`).  A listener of the wrong type
   triggers a `panic` at registration time rather than a silent
   misinterpretation at run time.

3. **Loose coupling.** Producers and consumers are connected by name
   strings resolved at initialization.  A SimObject that fires
   `"RetiredInsts"` never includes headers from the listener that counts
   them.

4. **Composability.** Multiple listeners can attach to the same probe
   point, and a single listener object can subscribe to probe points on
   many different SimObjects.

## Architecture Overview

```
  SimObject (producer)                 SimObject (consumer)
  ────────────────────                 ────────────────────
  ProbeManager                         (or ProbeListenerObject)
    │
    ├── ProbePointArg<DynInstPtr>      ProbeListenerArg<T, DynInstPtr>
    │      "Commit"  ──────notify()────►  T::handleCommit()
    │
    ├── ProbePointArg<PacketInfo>      ProbeListenerArgFunc<PacketInfo>
    │      "PktRequest" ──notify()────►  lambda callback
    │
    └── ProbePointArg<uint64_t>        (PMU counter)
           "Cycles"  ──────notify()────►  pmu.increment()
```

The three core classes are:

| Class | Role |
|---|---|
| `ProbePointArg<Arg>` | Named notification source owned by a SimObject. Holds a vector of listeners and calls `notify(arg)` on each. |
| `ProbeListenerArgBase<Arg>` | Abstract listener base.  Concrete forms bind either a member-function pointer (`ProbeListenerArg`) or a `std::function` (`ProbeListenerArgFunc`). |
| `ProbeManager` | Per-SimObject registry that maps probe-point names to `ProbePoint` objects and connects listeners by name. |

## Initialization Lifecycle

During `SimObject::init()` gem5 calls two virtual methods on every
SimObject, in hierarchical order:

1. **`regProbePoints()`** -- Each SimObject creates its
   `ProbePointArg<>` objects and registers them with its
   `ProbeManager`.

2. **`regProbeListeners()`** -- Listener objects look up probe points
   by name on other SimObjects' `ProbeManager` instances and attach
   themselves.

Listeners can also be added or removed **dynamically** at any point
during simulation.

## C++ API Reference

All classes live in `namespace gem5` and are declared in
`src/sim/probe/probe.hh`.

### ProbePoint / ProbePointArg\<Arg\>

```cpp
// Base (non-templated, for containers)
class ProbePoint {
    ProbePoint(ProbeManager *manager, const std::string &name);
    const std::string &getName() const;
    virtual void addListener(ProbeListener *listener) = 0;
    virtual void removeListener(ProbeListener *listener) = 0;
};

// Concrete templated form
template <typename Arg>
class ProbePointArg : public ProbePoint {
    ProbePointArg(ProbeManager *manager, std::string name);
    bool hasListeners() const;
    void notify(const Arg &arg);        // fires all listeners
    void addListener(ProbeListener *l);
    void removeListener(ProbeListener *l);
};
```

**Creating a probe point** (typically in `regProbePoints()` or the
constructor):

```cpp
// In your SimObject header:
ProbePointArg<DynInstPtr> *ppCommit = nullptr;

// In regProbePoints():
ppCommit = new ProbePointArg<DynInstPtr>(getProbeManager(), "Commit");
```

**Firing a probe point:**

```cpp
if (ppCommit->hasListeners())   // skip if nobody is listening
    ppCommit->notify(inst);
```

### ProbeListener / ProbeListenerArg / ProbeListenerArgFunc

```cpp
// Abstract base
class ProbeListener {
    ProbeListener(std::string name);
    const std::string &getName() const;
};

// Typed abstract base -- provides notify()
template <class Arg>
class ProbeListenerArgBase : public ProbeListener {
    virtual void notify(const Arg &val) = 0;
};

// Member-function-pointer variant
template <class T, class Arg>
class ProbeListenerArg : public ProbeListenerArgBase<Arg> {
    ProbeListenerArg(T *obj, std::string name,
                     void (T::*func)(const Arg &));
    void notify(const Arg &val) override;   // calls (obj->*func)(val)
};

// Lambda / std::function variant
template <class Arg>
class ProbeListenerArgFunc : public ProbeListenerArgBase<Arg> {
    ProbeListenerArgFunc(const std::string &name,
                         const std::function<void(const Arg &)> &func);
    void notify(const Arg &val) override;   // calls func(val)
};
```

### ProbeManager

Every `SimObject` owns a `ProbeManager` accessible via
`getProbeManager()`.

```cpp
class ProbeManager : public Named {
    // Connect a listener to the named probe point.
    bool addListener(std::string_view point_name, ProbeListener &listener);
    bool removeListener(std::string_view point_name, ProbeListener &listener);

    // Register a new probe point.
    void addPoint(ProbePoint &point);

    // Look up a probe point by name.
    ProbePoint *getFirstProbePoint(std::string_view point_name) const;

    // Create, connect, and return a RAII-managed listener.
    template <typename Listener, typename... Args>
    ProbeListenerPtr<Listener> connect(Args &&...args);
};
```

`ProbeListenerPtr<>` is a `std::unique_ptr` with a custom deleter that
automatically removes the listener from its probe point before
destruction.

### ProbeListenerObject

`ProbeListenerObject` (`src/sim/probe/probe_listener_object.hh`) is a
convenience `SimObject` base class for components whose sole purpose is to
listen to probes.  It owns a `ProbeManager` and a vector of
`ProbeListenerPtr<>` for lifetime management.

```cpp
class ProbeListenerObject : public SimObject {
    ProbeManager *getProbeManager();

    // Create and attach a listener (stored in `listeners` vector).
    template <typename T, typename... Args>
    void connectListener(Args &&...args);
};
```

## Shared Probe Types

gem5 pre-defines two probe-point type aliases in `namespace probing` for
cross-component use:

### PMU probes (`src/sim/probe/pmu.hh`)

```cpp
namespace probing {
    typedef ProbePointArg<uint64_t> PMU;
    typedef std::unique_ptr<PMU>    PMUUPtr;
}
```

Used for performance-counter instrumentation.  The `notify()` argument
is an event-count increment (usually 1).

### Packet probes (`src/sim/probe/mem.hh`)

```cpp
namespace probing {
    struct PacketInfo {
        MemCmd cmd;
        Addr addr;
        uint32_t size;
        Request::FlagsType flags;
        Addr pc;
        RequestorID id;
    };

    typedef ProbePointArg<PacketInfo> Packet;
    typedef std::unique_ptr<Packet>   PacketUPtr;
}
```

Used for memory-system instrumentation.  `PacketInfo` captures the
essential fields from a `Packet` so the original object can be safely
modified or deleted after the probe fires.

**Naming convention for packet probes:**

| Name | Meaning |
|---|---|
| `PktRequest` | Outgoing request on the memory side (or incoming for memories). |
| `PktResponse` | Incoming response on the memory side. |
| `PktRequestCPU` | Accepted request on the CPU side of a two-sided component (caches, crossbars). |
| `PktResponseCPU` | Outgoing response on the CPU side. |

### Cache probes (`src/mem/cache/cache_probe_arg.hh`)

```cpp
class CacheAccessProbeArg {
    PacketPtr pkt;          // the triggering packet
    CacheAccessor &cache;   // provides inCache(), inMissQueue(), etc.
};

struct CacheDataUpdateProbeArg {
    Addr addr;
    bool isSecure;
    RequestorID requestorID;
    std::vector<uint64_t> oldData;   // empty on fill
    std::vector<uint64_t> newData;   // empty on invalidation
    bool hwPrefetched;
    CacheAccessor &accessor;
};
```

## Catalog of Built-in Probe Points

### CPU probe points (BaseCPU)

Source: `src/cpu/base.hh`

| Probe name | Type | Description |
|---|---|---|
| `Cycles` | `probing::PMU` | All CPU cycles (including suspended). |
| `ActiveCycles` | `probing::PMU` | Cycles with at least one active thread. |
| `RetiredInsts` | `probing::PMU` | Committed instructions. |
| `RetiredInstsPC` | `probing::PMU` | Committed instructions (carries PC). |
| `RetiredLoads` | `probing::PMU` | Committed load instructions. |
| `RetiredStores` | `probing::PMU` | Committed store instructions. |
| `RetiredBranches` | `probing::PMU` | Committed branch instructions. |
| `Sleeping` | `ProbePointArg<bool>` | CPU sleep/wake transitions. |

### O3 CPU pipeline probe points

| Probe name | Type | Source |
|---|---|---|
| `Fetch` | `ProbePointArg<DynInstPtr>` | `src/cpu/o3/fetch.hh` |
| `FetchRequest` | `ProbePointArg<RequestPtr>` | `src/cpu/o3/fetch.hh` |
| `Rename` | `ProbePointArg<DynInstPtr>` | `src/cpu/o3/rename.hh` |
| `SquashInRename` | `ProbePointArg<SeqNumRegPair>` | `src/cpu/o3/rename.hh` |
| `Dispatch` | `ProbePointArg<DynInstPtr>` | `src/cpu/o3/iew.hh` |
| `Execute` | `ProbePointArg<DynInstPtr>` | `src/cpu/o3/iew.hh` |
| `Mispredict` | `ProbePointArg<DynInstPtr>` | `src/cpu/o3/iew.hh` |
| `ToCommit` | `ProbePointArg<DynInstPtr>` | `src/cpu/o3/iew.hh` |
| `Commit` | `ProbePointArg<DynInstPtr>` | `src/cpu/o3/commit.hh` |
| `CommitStall` | `ProbePointArg<DynInstPtr>` | `src/cpu/o3/commit.hh` |
| `Squash` | `ProbePointArg<DynInstPtr>` | `src/cpu/o3/commit.hh` |
| `InstAccessComplete` | `ProbePointArg<PacketPtr>` | `src/cpu/o3/cpu.hh` |
| `DataAccessComplete` | `ProbePointArg<pair<DynInstPtr, PacketPtr>>` | `src/cpu/o3/cpu.hh` |
| `FTQInsert` | `ProbePointArg<FetchTargetPtr>` | `src/cpu/o3/ftq.hh` |
| `FTQRemove` | `ProbePointArg<FetchTargetPtr>` | `src/cpu/o3/ftq.hh` |

### Branch predictor probe points

Source: `src/cpu/pred/bpred_unit.hh`

| Probe name | Type | Description |
|---|---|---|
| `Branches` | `probing::PMU` | Branch instructions processed. |
| `Misses` | `probing::PMU` | Branch mispredictions. |

### Cache probe points (BaseCache)

Source: `src/mem/cache/base.hh`

| Probe name | Type | Description |
|---|---|---|
| `Hit` | `ProbePointArg<CacheAccessProbeArg>` | Cache hit. |
| `Miss` | `ProbePointArg<CacheAccessProbeArg>` | Cache miss. |
| `Fill` | `ProbePointArg<CacheAccessProbeArg>` | Cache line fill. |
| `Data Update` | `ProbePointArg<CacheDataUpdateProbeArg>` | Data contents changed. |

### CommMonitor probe points

Source: `src/mem/comm_monitor.hh`

| Probe name | Type | Description |
|---|---|---|
| `PktRequest` | `probing::Packet` | Request packet forwarded. |
| `PktResponse` | `probing::Packet` | Response packet forwarded. |

### Thermal probe points

Source: `src/sim/power/thermal_domain.hh`

| Probe name | Type | Description |
|---|---|---|
| `thermalUpdate` | `ProbePointArg<Temperature>` | Temperature changed. |

### ARM TLB probe points

Source: `src/arch/arm/tlb.hh`

| Probe name | Type | Description |
|---|---|---|
| `InstRefills` | `probing::PMU` | Instruction TLB refills. |
| `DataRefills` | `probing::PMU` | Data TLB refills. |

## Built-in Probe Listeners

gem5 ships several ready-to-use listener SimObjects that can be attached
in Python configuration scripts.

### Memory probe listeners

All memory probe listeners inherit from `BaseMemProbe`
(`src/mem/probes/base.hh`), which supports attaching to **multiple**
`ProbeManager` instances and listens to a configurable probe name
(default `"PktRequest"`).

**MemTraceProbe** (`src/mem/probes/mem_trace.hh`) -- Records all
observed memory transactions to a compressed protobuf trace file.

```python
from m5.objects import CommMonitor, MemTraceProbe

system.monitor = CommMonitor()
system.monitor.trace = MemTraceProbe(
    trace_file="mem.ptrc.gz",
    trace_compress=True,
    with_pc=False,
)
```

**MemFootprintProbe** (`src/mem/probes/mem_footprint.hh`) -- Tracks
the set of unique addresses accessed at cache-line and page
granularity.  Exports statistics:

- `cacheLine` / `cacheLineTotal` -- current / cumulative line footprint
- `page` / `pageTotal` -- current / cumulative page footprint

```python
from m5.objects import CommMonitor, MemFootprintProbe

system.monitor = CommMonitor()
system.monitor.footprint = MemFootprintProbe(page_size=4096)
```

**StackDistProbe** (`src/mem/probes/stack_dist.hh`) -- Computes
reuse (stack) distance distributions for cache performance analysis.

```python
from m5.objects import CommMonitor, StackDistProbe

system.monitor = CommMonitor()
system.monitor.stackdist = StackDistProbe(
    line_size=64,
    linear_hist_bins=16,
    log_hist_bins=32,
)
```

### CPU probe listeners

**LocalInstTracker / GlobalInstTracker** (`src/cpu/probes/inst_tracker.hh`)
-- Count retired instructions across cores and trigger simulation exit
events at configurable thresholds.

```python
from m5.objects import GlobalInstTracker, LocalInstTracker

global_tracker = GlobalInstTracker(
    inst_thresholds=[100_000_000, 200_000_000]
)

for core in system.cpu:
    tracker = LocalInstTracker(
        global_inst_tracker=global_tracker,
        start_listening=True,
    )
    core.probeListener = tracker
```

**PcCountTracker / PcCountTrackerManager**
(`src/cpu/probes/pc_count_tracker.hh`) -- Count executions of specific
program-counter addresses and exit when a target count is reached.

```python
from m5.objects import PcCountTracker, PcCountTrackerManager

manager = PcCountTrackerManager(targets=[...])
tracker = PcCountTracker(
    targets=[...],
    core=cpu,
    ptmanager=manager,
)
cpu.probeListener = tracker
```

### O3 CPU trace listeners

**ElasticTrace** (`src/cpu/o3/probe/ElasticTrace.py`) -- Records
instruction-fetch and data-dependency traces for trace-driven replay.

```python
cpu.traceListener = ElasticTrace(
    instFetchTraceFile="fetch.proto.gz",
    dataDepTraceFile="dep.proto.gz",
    depWindowSize=3 * cpu.numROBEntries,
)
```

**SimpleTrace** (`src/cpu/o3/probe/SimpleTrace.py`) -- Lightweight
O3CPU trace listener for debugging.

## How-to: Writing a Custom Probe Point

### Step 1: Define the probe point in your SimObject

```cpp
// MyDevice.hh
#include "sim/probe/probe.hh"

class MyDevice : public SimObject
{
    ProbePointArg<uint32_t> *ppEvent = nullptr;

    void regProbePoints() override;
    void doSomething();
};
```

### Step 2: Register it

```cpp
// MyDevice.cc
void MyDevice::regProbePoints()
{
    ppEvent = new ProbePointArg<uint32_t>(
        getProbeManager(), "MyEvent");
}
```

### Step 3: Fire it

```cpp
void MyDevice::doSomething()
{
    uint32_t value = computeValue();
    if (ppEvent->hasListeners())
        ppEvent->notify(value);
}
```

## How-to: Writing a Custom Probe Listener

### Option A: Standalone listener SimObject

Inherit from `ProbeListenerObject` for a Python-configurable listener.

```cpp
// MyCounter.hh
#include "sim/probe/probe_listener_object.hh"

class MyCounter : public ProbeListenerObject
{
    uint64_t count = 0;
    void handleEvent(const uint32_t &val);

  public:
    using ProbeListenerObject::ProbeListenerObject;
    void regProbeListeners() override;
};
```

```cpp
// MyCounter.cc
void MyCounter::regProbeListeners()
{
    connectListener<ProbeListenerArg<MyCounter, uint32_t>>(
        this, "MyEvent", &MyCounter::handleEvent);
}

void MyCounter::handleEvent(const uint32_t &val)
{
    count += val;
}
```

### Option B: Lambda listener within an existing SimObject

Use `ProbeManager::connect()` for lightweight, inline listeners.

```cpp
void MyOtherDevice::regProbeListeners()
{
    auto listener = targetObject->getProbeManager()->
        connect<ProbeListenerArgFunc<uint32_t>>(
            "MyEvent",
            [this](const uint32_t &val) { handleIt(val); });
    // store `listener` to keep it alive
}
```

### Option C: Connecting via BaseMemProbe

For memory-system probes, inherit from `BaseMemProbe` and implement
`handleRequest()`.

```cpp
class MyMemAnalyzer : public BaseMemProbe
{
    void handleRequest(const probing::PacketInfo &pkt) override
    {
        // analyze pkt.addr, pkt.cmd, pkt.size, etc.
    }
};
```

In Python, attach it to one or more `CommMonitor` objects:

```python
system.monitor.analyzer = MyMemAnalyzer(
    manager=[system.monitor],
    probe_name="PktRequest",
)
```

## Debug Flags

| Flag | Purpose |
|---|---|
| `ProbeVerbose` | Verbose probe-system registration and notification messages. |
| `PcCountTracker` | PC count tracker debugging. |
| `InstTracker` | Instruction tracker debugging. |

Enable with `--debug-flags=ProbeVerbose` on the gem5 command line.

## Key Source Files

```
src/sim/probe/
    probe.hh                 Core classes (ProbePoint, ProbeListener, ProbeManager)
    probe.cc                 ProbeManager and ProbePoint implementations
    probe_listener_object.hh ProbeListenerObject base class
    Probe.py                 Python SimObject definition
    pmu.hh                   probing::PMU type alias
    mem.hh                   probing::Packet / PacketInfo type alias

src/mem/cache/
    cache_probe_arg.hh       CacheAccessProbeArg, CacheDataUpdateProbeArg

src/mem/probes/
    base.hh/cc               BaseMemProbe (multi-manager listener base)
    mem_trace.hh/cc          MemTraceProbe
    mem_footprint.hh/cc      MemFootprintProbe
    stack_dist.hh/cc         StackDistProbe

src/cpu/probes/
    inst_tracker.hh/cc       LocalInstTracker / GlobalInstTracker
    pc_count_tracker.hh/cc   PcCountTracker / PcCountTrackerManager

src/cpu/o3/probe/
    ElasticTrace.py/cc/hh   ElasticTrace listener
    SimpleTrace.py/cc/hh    SimpleTrace listener

src/mem/
    comm_monitor.hh/cc       CommMonitor (PktRequest / PktResponse probes)
```

## Trace Output Formats

Probe listeners that record traces write data in several formats.  This
section documents each format, its on-disk structure, and the utilities
available for decoding.

### Protobuf packet trace (MemTraceProbe)

Source: `src/mem/probes/mem_trace.cc`, proto: `src/proto/packet.proto`

MemTraceProbe writes a binary protobuf stream with optional gzip
compression (auto-selected when the filename ends in `.gz`).

**On-disk layout:**

```
[4 bytes: 0x356d6567 = ASCII "gem5"]   ← magic number
[varint32 length] [PacketHeader msg]   ← one header message
[varint32 length] [Packet msg]         ← repeated data messages
[varint32 length] [Packet msg]
...
[EOF]
```

**PacketHeader fields:**

| Field | Type | Description |
|---|---|---|
| `obj_id` | string | SimObject name of the trace source. |
| `tick_freq` | uint64 | Ticks per second (for time conversion). |
| `id_strings` | repeated string | Requestor-ID-to-name mapping. |

**Packet fields:**

| Field | Type | Description |
|---|---|---|
| `tick` | uint64 | Simulation timestamp. |
| `cmd` | uint32 | Memory command (1 = ReadReq, 4 = WriteReq, ...). |
| `addr` | uint64 | Physical address. |
| `size` | uint32 | Access size in bytes. |
| `flags` | uint32 | Request flags. |
| `pkt_id` | uint64 | Packet identifier. |
| `pc` | uint64 | Program counter (present only if `with_pc=True`). |

**Decoder:**

```bash
util/decode_packet_trace.py m5out/mem.ptrc.gz
```

Outputs CSV: `[pkt_id,] cmd, addr, size, [flags,] tick [,pc]`

**Encoder** (for trace-driven replay):

```bash
util/encode_packet_trace.py input.csv output.ptrc.gz
```

### Protobuf instruction dependency trace (ElasticTrace)

Source: `src/cpu/o3/probe/elastic_trace.cc`,
proto: `src/proto/inst_dep_record.proto`

ElasticTrace produces **two** protobuf files:

1. **Instruction fetch trace** -- uses the same `Packet` message format
   as MemTraceProbe (records I-cache requests).

2. **Data dependency trace** -- uses the `InstDepRecord` message.

**InstDepRecord fields:**

| Field | Type | Description |
|---|---|---|
| `seq_num` | uint64 | Instruction sequence number. |
| `type` | enum | `INVALID`, `LOAD`, `STORE`, or `COMP`. |
| `p_addr` | uint64 | Physical address (LOAD/STORE only). |
| `size` | uint32 | Memory access size. |
| `flags` | uint32 | Memory request flags. |
| `rob_dep` | repeated uint64 | Order (ROB) dependencies. |
| `comp_delay` | uint64 | Computational delay in ticks. |
| `reg_dep` | repeated uint64 | Register data dependencies. |
| `weight` | uint32 | Collapsed node count. |
| `pc` | uint64 | Program counter. |
| `v_addr` | uint64 | Virtual address (optional). |
| `asid` | uint32 | Address space ID. |

**Decoder:**

```bash
util/decode_inst_dep_trace.py m5out/dep.proto.gz
```

**Encoder:**

```bash
util/encode_inst_dep_trace.py input.ascii output.proto.gz
```

### Protobuf instruction execution trace

Proto: `src/proto/inst.proto`

Used by `ExeTracer` and related instruction tracers.  Each message
records one executed instruction.

**Inst fields:**

| Field | Type | Description |
|---|---|---|
| `pc` | uint64 | Program counter. |
| `inst` | fixed32 | Instruction encoding (fixed-width ISAs). |
| `inst_bytes` | bytes | Variable-length instruction bytes. |
| `nodeid` | uint32 | NUMA node ID. |
| `cpuid` | uint32 | CPU ID. |
| `tick` | fixed64 | Execution timestamp. |
| `type` | enum | `IntAlu`, `IntMul`, `FloatAdd`, `MemRead`, `MemWrite`, etc. |
| `mem_access` | repeated | Memory accesses: `{addr, size, mem_flags}`. |

**Decoder:**

```bash
util/decode_inst_trace.py m5out/inst.proto.gz
```

Output format:
```
tick: (node/cpu) 0xINSTBITS @ 0xPC : Type {#addr-#addr+size;}
```

### SimPoint basic-block vector (BBV) format

Source: `src/cpu/simple/probes/simpoint.cc`

Plain-text file with one line per profiling interval.  Each line lists
the basic blocks executed in that interval and their counts:

```
T:bb_id_1:count_1 :bb_id_2:count_2 :bb_id_3:count_3
T:bb_id_1:count_4 :bb_id_5:count_5
```

Consumed by the [SimPoint](https://cseweb.ucsd.edu/~calder/simpoint/)
tool to select representative simulation intervals.

### Protobuf I/O infrastructure

All protobuf traces share a common I/O layer:

- **C++:** `ProtoOutputStream` / `ProtoInputStream`
  (`src/proto/protoio.hh`)
- **Python:** `util/protolib.py` -- `openFileRd()`, `decodeMessage()`,
  `encodeMessage()`, varint32 helpers
- **Magic number:** `0x356d6567` ("gem5")
- **Compression:** gzip, auto-detected by `.gz` file extension

### Statistics output formats

gem5's statistics system (separate from probes but often used alongside
them) supports three output formats:

| Format | Class | File | Notes |
|---|---|---|---|
| **Text** | `statistics::Text` | `src/base/stats/text.cc` | Default.  Human-readable `stats.txt`. |
| **HDF5** | `statistics::Hdf5` | `src/base/stats/hdf5.cc` | Binary, time-series capable.  Requires HDF5 library. |
| **JSON** | via Python | `src/python/m5/ext/pystats/` | Programmatic access via `gem5stats.py`. |

HDF5 is particularly useful for probe-driven workflows: attach probes
that update stats, configure periodic dumps, then analyze the time-series
in Python with `h5py` or `pandas`.

## Visualization and Analysis Tools

### Pipeline visualizers

**O3PipeView** (built-in, text) -- `util/o3-pipeview.py`

Renders a text-mode timeline of the O3CPU pipeline.  Each instruction
shows its stage progression: fetch (f), decode (d), rename (n),
dispatch (p), issue (i), complete (c), retire (r).

```bash
# Generate the trace
./build/ALL/gem5.opt --debug-flags=O3PipeView \
    --debug-start=1000 --debug-file=trace.out configs/...

# Render it
./util/o3-pipeview.py -c 500 --color m5out/trace.out
```

**Konata** (third-party, GUI) --
[github.com/shioyadan/Konata](https://github.com/shioyadan/Konata)

A graphical pipeline viewer described as "Google Maps for an out-of-order
pipeline."  Provides smooth pan/zoom navigation over pipeline activity.
Cross-platform (Electron-based).  Open gem5 `O3PipeView` trace files
directly.

- Pre-built binaries:
  [releases](https://github.com/shioyadan/Konata/releases)
- Tutorial:
  [gem5-konata.pdf](https://raw.githubusercontent.com/wiki/shioyadan/Konata/gem5-konata.pdf)
- Demo: [Visualizing Spectre with
  gem5](http://www.lowepower.com/jason/visualizing-spectre-with-gem5.html)

**MinorView** (built-in, GUI) -- `util/minorview.py`

Graphical cycle-by-cycle visualizer for the Minor (in-order) CPU model.
Requires the `MinorTrace` debug flag.

```bash
./build/ALL/gem5.opt --debug-flags=MinorTrace configs/...
./util/minorview.py --picture=src/minor/minor.pic m5out/trace.out
```

### System topology visualization

**Graphviz DOT writer** (built-in) --
`src/python/m5/util/dot_writer.py`

Automatically generates `config.dot.pdf` and `config.dot.svg` in the
output directory when `pydot` is installed.  Nodes are SimObjects; edges
represent the memory hierarchy.

### Memory and DRAM visualization

**plot_dram scripts** (built-in) -- `util/plot_dram/`

- `dram_sweep_plot.py` -- 3D surface plots of bandwidth vs. latency
- `dram_lat_mem_rd_plot.py` -- DRAM latency characteristics
- `lowp_dram_sweep_plot.py` -- Low-power DRAM (LPDDR) sweeps
- `PlotPowerStates.py` -- DRAM power state transitions over time

Requires `matplotlib` and `numpy`.

### Network-on-chip analysis

**DSENT integration** (built-in) --
`util/on-chip-network-power-area.py`

Computes power and area estimates for on-chip network routers and links
using the DSENT tool (`ext/dsent/`).  Works with Garnet 2.0 network
statistics.

**Loupe** (third-party) --
[github.com/dhcabinian/Loupe](https://github.com/dhcabinian/Loupe)

Network-on-chip visualization for Garnet 2.0 mesh networks.

### ARM Streamline integration

**m5stats2streamline.py** (built-in) -- `util/streamline/`

Converts gem5 output to ARM DS-5 Streamline `.apc` format for rich
GUI-based timeline visualization of performance counters, process/thread
activity, and power states.  ARM ISA only.

### Power modeling

**McPAT parsers** (third-party) -- Several community tools bridge gem5
statistics to McPAT input for power/area/timing analysis:

- [Gem5toMcPat_parser](https://github.com/Hardik44/Gem5toMcPat_parser)
- [McPAT-Calib](https://github.com/zhaijw18/mcpat-calib-public) (ML-calibrated)

gem5 also includes a built-in `PowerModel` / `MathExprPowerModel`
framework for defining power equations directly in Python configuration
scripts.

### Statistics post-processing

**gem5-stat-processing** --
[github.com/mahyarsamani/gem5-stat-processing](https://github.com/mahyarsamani/gem5-stat-processing)

Python package for parsing `stats.txt` and preparing data for
visualization.

**gem5 Sim Viewer** --
[felker.dev/gem5-sim-viewer](https://felker.dev/gem5-sim-viewer/)

Browser-based tool that reads your `m5out/` directory and displays an
interactive accordion tree of statistics plus the system topology
diagram.

### Trace comparison utilities

| Tool | Location | Description |
|---|---|---|
| `tracediff` | `util/tracediff` | Diff two gem5 runs side by side. |
| `rundiff` | `util/rundiff` | Streaming diff for long traces. |
| `statetrace` | `util/statetrace/` | Compare gem5 against real hardware instruction-by-instruction via ptrace. |

### Summary

| What you want to do | Tool |
|---|---|
| Visualize O3 pipeline (text) | `util/o3-pipeview.py` |
| Visualize O3 pipeline (GUI) | [Konata](https://github.com/shioyadan/Konata) |
| Visualize Minor pipeline | `util/minorview.py` |
| Decode memory traces | `util/decode_packet_trace.py` |
| Decode instruction traces | `util/decode_inst_trace.py` |
| Decode dependency traces | `util/decode_inst_dep_trace.py` |
| View system topology | `m5out/config.dot.svg` (auto-generated) |
| Analyze DRAM behavior | `util/plot_dram/*.py` |
| Analyze NoC power/area | `util/on-chip-network-power-area.py` |
| Time-series stats analysis | HDF5 output + `h5py` / `pandas` |
| Browse stats interactively | [gem5 Sim Viewer](https://felker.dev/gem5-sim-viewer/) |
| Power estimation | McPAT parsers or built-in `PowerModel` |
| ARM performance analysis | `util/streamline/m5stats2streamline.py` |
| Compare two simulation runs | `util/tracediff` |
| Validate against real HW | `util/statetrace/` |
