# Plan: Integrate FTR Transaction Tracing into gem5 and Ruby

## Table of Contents

- [Problem Statement](#problem-statement)
- [Goals](#goals)
- [Non-Goals (First Version)](#non-goals-first-version)
- [Data Model](#data-model)
  - [Streams and Generators](#streams-and-generators)
  - [Transaction Nodes](#transaction-nodes)
  - [Transaction Events](#transaction-events)
  - [Relations](#relations)
  - [Static Attributes Per Node Kind](#static-attributes-per-node-kind)
- [Trace Identity: Where It Lives](#trace-identity-where-it-lives)
  - [TraceContext Extension on Request](#tracecontext-extension-on-request)
  - [m_rootTraceId on Message](#m_roottraceid-on-message)
  - [m_traceId on flit](#m_traceid-on-flit)
- [Transaction Lifecycle](#transaction-lifecycle)
  - [Flit Lifecycle](#flit-lifecycle)
- [Instrumentation Points](#instrumentation-points)
  - [Automatic: Framework Primitives](#automatic-framework-primitives)
  - [Automatic: Garnet Router ProbePoints](#automatic-garnet-router-probepoints)
  - [Manual: Domain-Specific](#manual-domain-specific)
  - [SimpleNetwork](#simplenetwork)
- [FtrTrace SimObject](#ftrtrace-simobject)
  - [Singleton Access](#singleton-access)
  - [Root Creation Filter Policy](#root-creation-filter-policy)
  - [Recorder API](#recorder-api)
- [Implementation Steps](#implementation-steps)
- [Risks and Mitigations](#risks-and-mitigations)
- [Future Extensions](#future-extensions)
- [Appendix: FTR Background](#appendix-ftr-background)

## Problem Statement

gem5 has statistics, debug printing, and some specialized trace outputs, but no coherent transaction trace that follows a memory request from the CPU-facing Ruby entry point through controllers, queues, network flits, and final retirement.

For Ruby work, that missing continuity makes it hard to answer:

- Where did this request stall?
- Which SimObject owned it at each point in time?
- When did one logical request fan out into multiple flits?
- How do we correlate a CPU-visible miss with the exact Garnet path that carried it?

The immediate goal is to trace the memory subsystem, starting at CPU requests entering Ruby and ending when the original request retires.
The longer-term goal is broader: the same framework should later support tracing instruction lifetimes in an O3 pipeline without inventing a second system.

## Goals

1. One top-level FTR transaction for every CPU request accepted into Ruby.
2. Keep that transaction alive until the request retires back toward the CPU.
3. Child transactions when a Ruby message is split into Garnet flits.
4. Timestamps when transactions hit important SimObjects.
5. Stable attributes: address, request type, size, requestor, and related identifiers.
6. Dotted hierarchical names from canonical `SimObject::name()`.
7. Explicit parent-child relationships between request and flit transactions.
8. Efficient enough for real Ruby experiments.
9. Generic enough to trace future O3 instruction transactions.

## Non-Goals (First Version)

1. Capturing every internal micro-event in Ruby.
2. Solving all protocol-specific message correlation.
3. Building a viewer.
4. Replacing existing statistics or debug flags.
5. Recording values as waveforms.

---

## Data Model

### Streams and Generators

A **stream** is a named timeline corresponding to a transaction originator — the component where transactions are born.
Intermediate components (controllers, message buffers, routers, links) appear as event attributes on transactions, not as separate streams.

A **generator** is a named source of transactions within a stream.

For a 4-core MESI Two Level system with Garnet:

| Stream name | Kind | Generators |
|-------------|------|------------|
| `system.ruby.l1_cntrl0.sequencer` | `Sequencer` | `memreq`, `flit` |
| `system.ruby.l1_cntrl1.sequencer` | `Sequencer` | `memreq`, `flit` |
| `system.ruby.l1_cntrl2.sequencer` | `Sequencer` | `memreq`, `flit` |
| `system.ruby.l1_cntrl3.sequencer` | `Sequencer` | `memreq`, `flit` |

The `memreq` generator produces root `MemoryRequest` transactions.
The `flit` generator produces `Flit` child transactions.
Both live on the same stream so that a request and its flit children appear together on the same viewer swim-lane.

Components that do **not** get their own stream (they appear only as event attributes):
cache controllers, message buffers, network interfaces, routers, links, memory controllers.

### Transaction Nodes

A transaction node is a time-bounded entity with a globally unique ID.

| Field | Description |
|-------|-------------|
| `trace_id` | Globally unique 64-bit ID for this node |
| `root_trace_id` | ID of the root ancestor (equals `trace_id` for root nodes) |
| `parent_trace_id` | ID of the immediate parent (zero for root nodes) |
| `kind` | `MemoryRequest` or `Flit` (future: `Instruction`, `RubyMessage`, etc.) |
| `begin_tick` | Tick when the transaction starts |
| `end_tick` | Tick when the transaction retires |
| `creation_object` | `SimObject::name()` where created |
| `retirement_object` | `SimObject::name()` where retired |
| `static_attributes` | Key-value map set at creation, not changed afterward |

One global 64-bit monotonic ID space for all node kinds.
No separate ID spaces per kind.

**Root `MemoryRequest` node:** `trace_id == root_trace_id`, `parent_trace_id == 0`.

**`Flit` child node:** own unique `trace_id`, root request's ID as both `root_trace_id` and `parent_trace_id` (in v1, flits attach directly to the root request).

### Transaction Events

Timestamped events attached to a transaction node.

| Field | Description |
|-------|-------------|
| `tick` | When it happened |
| `trace_id` | Which node this event belongs to |
| `event_kind` | What happened (e.g. `port_crossing`, `enqueue`, `dequeue`, `router_arrive`, `switch_alloc`, `switch_traverse`, `link_traverse`, `issue`, `completion`) |
| `object_name` | `SimObject::name()` where it happened |
| `stage_name` | Optional sub-object stage |
| `attributes` | Optional key-value pairs (queue occupancy, VC, outport, etc.) |

Flit pipeline stages (router arrival, switch allocation, crossbar traverse, link traverse) are events stamped onto the flit's own transaction node — not separate nodes.

### Relations

A relation is a named directed edge between two transactions, possibly across streams.
Used to link a root `MemoryRequest` to its `Flit` children within the same originator stream.

### Static Attributes Per Node Kind

**Root `MemoryRequest`** (read from `Request` at finalization, not duplicated into `TraceContext`):

- Physical address, line address, request size
- Command / Ruby request type
- Requestor ID, context ID, task ID
- Stream ID, substream ID (if present)
- PC, instruction sequence number (if present)
- Flags: secure, prefetch, atomic, LLSC, HTM, TLBI

**`Flit`:**

- Flit index, Garnet packet ID, flit type
- Width, vnet, VC (if known)
- Source NI name, destination node
- Route metadata (if available)

---

## Trace Identity: Where It Lives

Trace identity must survive across three object layers: `Request` (port world), `Message` (Ruby world), and `flit` (Garnet world).

### `TraceContext` Extension on `Request`

`Request` is `Extensible<Request>`, which is the cleanest place to attach durable trace metadata.
`TraceContext` carries only trace-specific fields not already on `Request`:

```cpp
struct TraceContext : public Extension<Request>
{
    TraceId trace_id;        // this node's unique ID
    TraceId root_trace_id;   // root ancestor (== trace_id for roots)
    TraceId parent_trace_id; // immediate parent (0 for roots)
    Tick origin_tick;        // when the root was created
};
```

Domain attributes (address, requestor ID, PC, etc.) are read directly from `Request` when needed.

### `m_rootTraceId` on `Message`

Ruby's `Message` class does not carry a `Request*`.
Only `RubyRequest` has `m_pkt` pointing back to the original packet.
Protocol response and forwarded messages do not.

Add one field to the `Message` base class:

```cpp
class Message
{
    TraceId m_rootTraceId = 0;  // 0 = untraced; root request trace ID
};
```

This always means "which root request tree does this message belong to?"
It does not identify a distinct message-level transaction node.

Propagation rules:
1. `RubyRequest` creation: copy from `TraceContext` on `pkt->req`.
2. `Message::clone()`: automatic — `m_rootTraceId` is a base field.
3. Newly constructed protocol messages (not cloned): `m_rootTraceId == 0` in v1. Later phases add explicit propagation per protocol.

Cost: one 64-bit field per `Message`.

### `m_traceId` on `flit`

Each flit gets its own transaction node, so it carries its own unique node ID:

```cpp
class flit
{
    TraceId m_traceId = 0;  // this flit's unique node ID
};
```

Set during `flitisizeMessage()` by allocating a new child node under the root request ID from `msg_ptr->m_rootTraceId`.

Cost: one 64-bit field per `flit`.

---

## Transaction Lifecycle

A request's trace lifecycle has four phases, each owned by one component:

```
Port hook (sendReq)          Sequencer::makeRequest()
    │                              │
    ▼                              ▼
[1. PENDING]  ──────────────► [2. LIVE]
  TraceContext attached           Root transaction finalized,
  to pkt->req;                    static attributes recorded,
  root node created               m_rootTraceId set on RubyRequest
  in pending state

                              Sequencer::readCallback() /
                              writeCallback() / atomicCallback()
                                   │
                                   ▼
                              [3. RETIRED]
                                Root transaction closed with
                                hit/miss type and data source

Port hook (sendResp)
    │
    ▼
[4. FINAL PORT CROSSING]
  Last port-crossing event stamped;
  no state change on node
```

**Phase 1 — Pending.**
`TimingRequestProtocol::sendReq()` detects a request entering a traced subsystem for the first time (no `TraceContext` on `pkt->req`, filter policy matches).
It creates a pending root node and attaches `TraceContext` to `pkt->req`.
If Ruby rejects the request for retry, the pending context is removed — no committed transaction leaks.

**Phase 2 — Live.**
`Sequencer::makeRequest()` finalizes the pending root as live.
Records static attributes (address, type, size, flags) by reading `pkt->req`.
Sets `m_rootTraceId` on the `RubyRequest` being created, bridging trace identity into Ruby's `Message` layer.

**Phase 3 — Retired.**
`Sequencer::readCallback()` / `writeCallback()` / `atomicCallback()` stamps a completion event with hit/miss information and retires the root transaction.

**Phase 4 — Final port crossing.**
The response crosses back through ports.
Each crossing is stamped as a `port_crossing` event.
No node state change — retirement already happened at the Sequencer.

### Flit Lifecycle

Flit child transactions are simpler:

1. **Created** in `NetworkInterface::flitisizeMessage()` with a fresh `trace_id`, parent set to root request.
2. **Events stamped** as the flit traverses routers and links (router arrive, switch alloc, crossbar traverse, link traverse — all events on the flit transaction).
3. **Retired** at destination `NetworkInterface` ejection.

---

## Instrumentation Points

All instrumentation falls into two categories: automatic (framework primitives, cover all current and future components) and manual (domain-specific, three hook points).

### Automatic: Framework Primitives

| # | Primitive | Location | What it covers |
|---|-----------|----------|---------------|
| 1 | `TimingRequestProtocol::sendReq()` | `src/mem/protocol/timing.cc` | Every timing request port crossing system-wide |
| 2 | `TimingResponseProtocol::sendResp()` | `src/mem/protocol/timing.cc` | Every timing response port crossing system-wide |
| 3 | `MessageBuffer::enqueue()` | `src/mem/ruby/network/MessageBuffer.cc` | Every Ruby queue enqueue, all protocols |
| 4 | `MessageBuffer::dequeue()` | `src/mem/ruby/network/MessageBuffer.cc` | Every Ruby queue dequeue, all protocols |
| 5 | `NetworkInterface::flitisizeMessage()` | Garnet `NetworkInterface.cc` | Every flit creation |
| 6 | `NetworkInterface::wakeup()` (dest) | Garnet `NetworkInterface.cc` | Every flit ejection |

### Automatic: Garnet Router ProbePoints

| # | Stage | ProbePoint name | Payload |
|---|-------|----------------|---------|
| 7 | `InputUnit::wakeup()` | `probeRouterArrive` | flit trace ID, router name, tick, VC |
| 8 | `SwitchAllocator::wakeup()` | `probeSwitchAlloc` | flit trace ID, router name, tick, VC, outport |
| 9 | `CrossbarSwitch::wakeup()` | `probeSwitchTraverse` | flit trace ID, router name, tick |
| 10 | `NetworkLink::wakeup()` | `probeLinkTraverse` | flit trace ID, link name, tick |

These use `ProbePointArg<FlitTraceStamp>` and fire to zero listeners (negligible cost) when `FtrTrace` is not instantiated.

### Manual: Domain-Specific

| # | Hook point | What it does |
|---|-----------|-------------|
| 11 | `Sequencer::makeRequest()` | Finalize pending root as live; record static attributes; set `m_rootTraceId` on `RubyRequest` |
| 12 | `Sequencer::issueRequest()` | Stamp issue event with protocol-specific details |
| 13 | `Sequencer::readCallback()` / `writeCallback()` / `atomicCallback()` | Stamp completion event; retire root transaction |

**Total: 13 hook points.** 10 automatic (framework-level), 3 manual (Sequencer-specific).

### SimpleNetwork

SimpleNetwork has no flits, routers, or `flitisizeMessage()`.
The `MessageBuffer` hooks (points 3-4) provide automatic queue-level tracing.
No flit child transactions are created.
If finer-grain tracing is needed later, add ProbePoints to `PerfectSwitch` and `Throttle`.

---

## `FtrTrace` SimObject

`FtrTrace` is the runtime recorder, implemented as a `SimObject`.

It owns: the output file, the monotonic ID allocator (`std::atomic<uint64_t>` for thread safety), the live transaction table, configuration, and final serialization.

### Singleton Access

```cpp
class FtrTrace : public SimObject
{
    static FtrTrace *instance;
  public:
    static FtrTrace *get() { return instance; }
};
```

All primitive-level hooks check `FtrTrace::get()` first.
If null, tracing is disabled — a single branch, effectively zero cost.
Same pattern as `Debug::SimpleFlag::tracing` for DPRINTF.

### Root Creation Filter Policy

The port-level hook does not know whether it is entering Ruby, Classic caches, or another subsystem.
`FtrTrace` owns a filter policy that decides which requests get root transactions:

```cpp
ftrTrace->setRootFilter([](PacketPtr pkt, ResponsePort *peer) {
    return dynamic_cast<RubyPort::MemResponsePort*>(peer) != nullptr
           && !pkt->req->hasExtension<TraceContext>();
});
```

Registered by `RubyPort` during `regProbeListeners()`.
Later, additional filters can be added for Classic caches, DMA, or O3 LSQ entry points.

### Recorder API

```cpp
TraceId createRootTransaction(const RequestPtr &req,
                              std::string_view kind,
                              std::string_view object_name,
                              Tick tick,
                              const TransactionAttrs &attrs);

TraceId createChildTransaction(TraceId parent,
                               std::string_view kind,
                               std::string_view object_name,
                               Tick tick,
                               const TransactionAttrs &attrs);

void stampEvent(TraceId id,
                std::string_view event_kind,
                std::string_view object_name,
                Tick tick,
                const EventAttrs &attrs = {});

void retireTransaction(TraceId id,
                       std::string_view object_name,
                       Tick tick,
                       const EventAttrs &attrs = {});
```

---

## Implementation Steps

### Step 1. Generic Trace Core

Create `src/sim/transaction_trace/` with:
- `FtrTrace.py`, `ftr_trace.hh`, `ftr_trace.cc`
- Common header for trace IDs and attribute types
- Build integration (SConscript) and one debug flag

Implement: file lifecycle, ID allocator, live transaction table, node creation/stamping/retirement API, flush on exit and drain-safe flush.

### Step 2. Trace Identity Fields

**2a.** Define `TraceContext` extension on `Request` (fields: `trace_id`, `root_trace_id`, `parent_trace_id`, `origin_tick`).

**2b.** Add `TraceId m_rootTraceId = 0` to `Message` base class.

**2c.** Add `TraceId m_traceId = 0` to `flit` class.

### Step 3. Port-Level Instrumentation

Add FTR calls to `TimingRequestProtocol::sendReq()` and `TimingResponseProtocol::sendResp()`.

Request path:
1. `FtrTrace::get()` null → skip.
2. `pkt->req` has `TraceContext` → stamp port-crossing event.
3. No `TraceContext` + filter matches → create pending root, attach `TraceContext`.

Response path:
1. `FtrTrace::get()` null or no `TraceContext` → skip.
2. Stamp port-crossing event. (Retirement is handled by Sequencer callback in Step 4.)

After this step, every port crossing in the system is traced automatically.

### Step 4. Sequencer Hooks

Three manual hooks:
1. `makeRequest()`: finalize pending root as live, record static attributes, set `m_rootTraceId` on `RubyRequest`.
2. `issueRequest()`: stamp issue event.
3. `readCallback()` / `writeCallback()` / `atomicCallback()`: stamp completion, retire root.

After this step, every Ruby request has a useful end-to-end lifetime trace (attributes, port crossings, issue, completion) before any Garnet work.

### Step 5. MessageBuffer Instrumentation

Add trace-awareness to `MessageBuffer::enqueue()` and `dequeue()`:

```cpp
void MessageBuffer::enqueue(MsgPtr message, ...)
{
    // ... existing logic ...
    if (auto *recorder = FtrTrace::get();
        recorder && message->m_rootTraceId != 0) {
        recorder->stampEvent(message->m_rootTraceId, "enqueue",
                             name(), curTick(),
                             {{"occupancy", m_prio_heap.size()},
                              {"vnet", message->getVnet()}});
    }
}
```

Covers all queue timestamps for all protocols and all controllers.
Messages with `m_rootTraceId == 0` are skipped at zero cost.

### Step 6. NetworkInterface Flit Children

In `flitisizeMessage()`: create one `Flit` child transaction per flit, record parent-child relation, stamp creation event, record static attributes (flit index, packet ID, vnet, VC, width, source NI, destination node).

In destination-side `wakeup()`: stamp ejection event, retire flit transaction.

### Step 7. Router Stage ProbePoints

Add one `ProbePointArg<FlitTraceStamp>` to each of the four Garnet pipeline stages.
`FtrTrace` registers as listener during `regProbeListeners()`.
Each probe stamps an event on the flit's transaction node.

### Step 8. Protocol-Aware Message Nodes (Later)

After primitive-level instrumentation is proven useful, optionally add `RubyMessage` child nodes at key protocol emission sites.
Start with MESI Two Level.
This is the only step requiring per-protocol work.

### Step 9. Validation and Documentation

Unit tests: ID allocation, parent-child relationships, retirement, serialization.

Integration tests: one root per accepted request, port-crossing events appear automatically, queue events appear for traced messages, flit child count matches expected decomposition, `m_rootTraceId` survives `clone()`.

Smoke config: small FTR file from a Ruby random test or synthetic Garnet traffic run.

Human inspection: open trace in SCViewer, verify dotted names match SimObject hierarchy.

---

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| Protocol coupling | Primitive-level instrumentation eliminates it for port crossings, queues, and network events. Only 3 Sequencer hooks touch protocol-specific code. |
| Runtime overhead | Zero-cost-when-disabled: null pointer check on `FtrTrace::get()`, null `m_rootTraceId` skip in MessageBuffer, zero-listener ProbePoints. |
| Pointer-lifetime bugs | Durable identity on `Request` extension and recorder IDs, not transient pointers. |
| Trace explosion on large networks | Event filtering by node kind, object path prefix, vnet, or event kind. Selective ProbePoint registration per router group. |
| Hard to extend to O3 | Port-level hooks already cover O3 LSQ requests automatically. O3 pipeline tracing adds new ProbePoints at pipeline stages, same pattern as Garnet. |

---

## Future Extensions

**O3 instruction tracing.**
Instruction transactions originate at the CPU fetch stage on a separate stream (e.g. `system.cpu0.fetch`) with an `instruction` generator.
A memory-request transaction on the sequencer stream becomes a child of the instruction transaction via an explicit FTR relation.
The same event vocabulary (creation, issue, completion, retirement) maps onto pipeline stages.

**CHI protocol.**
Same stream-per-originator pattern: stream named after the RNF's sequencer (e.g. `system.ruby.hnf00.cntrl.sequencer`).

**Perfetto backend.**
The internal trace model (hierarchical spans, parent-child relations, key-value attributes) maps naturally to Perfetto's protobuf format.
A narrow writer interface allows adding a Perfetto backend without changing instrumentation code.
Evaluate after the first vertical slice is working.

---

## Appendix: FTR Background

### What Is a Transaction Trace?

A transaction trace bridges the gap between low-level simulator events and the questions engineers actually ask about transactions: "Where did this request stall?", "How long did the L2 miss take end-to-end?"

Every transaction trace builds on four concepts:

1. **Transaction (Span/Slice).** A time-bounded event with begin/end timestamps, a name, and a parent context.
2. **Stream (Track/Service).** A named timeline grouping related transactions, corresponding to a transaction originator.
3. **Attributes (Properties/Annotations).** Key-value metadata attached to a transaction.
4. **Relations (Links/Flows).** Directed edges between transactions expressing causality.

### Why FTR?

FTR (Fast Transaction Recording) was created by MINRES Technologies as part of the SystemC-Components (SCC) project.
It is the modern high-performance backend for SystemC SCV transaction recording.

- **Open source** (Apache 2.0)
- **Hardware-oriented data model** — streams, generators, typed attributes, explicit relations
- **Compact binary format** — CBOR encoding with LZ4 compression and string interning
- **Streaming writes** — chunks flushed during simulation, no unbounded accumulation
- **Viewable in SCViewer** — open-source transaction viewer

### FTR File Layout

```
┌─────────────────────────────────────────────────────┐
│                   FTR File Layout                    │
├──────────┬──────────────────────────────────────────┤
│ Info     │ Timescale (e.g. 10⁻¹² = picoseconds)    │
├──────────┼──────────────────────────────────────────┤
│ Dictionary│ Interned string table (id → string)     │
├──────────┼──────────────────────────────────────────┤
│ Directory│ Stream and generator definitions          │
├──────────┼──────────────────────────────────────────┤
│ TX Blocks│ Batches of transactions per stream,      │
│          │ each with: id, generator, start/end time,│
│          │ typed attributes (BEGIN/RECORD/END)       │
├──────────┼──────────────────────────────────────────┤
│ Relations│ Named directed edges between transactions│
└──────────┴──────────────────────────────────────────┘
```

### FTR Writer API

```cpp
ftr::ftr_writer<true> trace("output.ftr");  // compressed
trace.writeInfo(-12);                        // picoseconds
trace.writeStream(0, "system.ruby.l1_cntrl0.sequencer", "Sequencer");
trace.writeGenerator(0, "memreq", 0);
trace.writeGenerator(1, "flit",   0);

uint64_t tx = 1;
trace.startTransaction(tx, /*gen=*/0, /*stream=*/0, /*time=*/1000);
trace.writeAttribute(tx, ftr::event_type::BEGIN,
    "address", ftr::data_type::UNSIGNED, uint64_t(0x80001000));
trace.endTransaction(tx, /*time=*/1045);

uint64_t flit_tx = 2;
trace.startTransaction(flit_tx, /*gen=*/1, /*stream=*/0, /*time=*/1010);
trace.endTransaction(flit_tx, /*time=*/1040);

trace.writeRelation("parent_of", /*sink_stream=*/0, /*sink_tx=*/2,
                                  /*src_stream=*/0,  /*src_tx=*/1);
```

### Transaction Traces in Other Systems

| System | Span concept | Timeline concept | Causality concept |
|--------|-------------|-----------------|-------------------|
| FSDB (Synopsys Verdi) | Transaction | Stream | Parent handle |
| SystemC SCV | Transaction | Stream/Generator | Parent handle |
| OpenTelemetry | Span | Service | Parent span ID, span links |
| Perfetto | Slice | Track | Flow ID |

FTR inherits the SCV vocabulary directly and adds compact binary serialization with LZ4 compression.
