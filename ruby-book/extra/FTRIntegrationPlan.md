# Plan: Integrate FTR Transaction Tracing into gem5 and Ruby

## Background: Transaction Traces and the FTR Format

### What Is a Transaction Trace?

Hardware simulators produce enormous volumes of low-level event data — signal toggles, cache-line state changes, queue enqueue/dequeue operations — but the questions engineers actually ask are about *transactions*: "Where did this memory request stall?", "How long did the L2 miss take end-to-end?", "Which path through the network carried the response?"

A **transaction trace** bridges this gap.
It records the life of a logical operation — a memory request, a bus transfer, a network packet — as a single named, timed entity that carries attributes and participates in causal relationships with other transactions.

Every transaction trace, regardless of the specific format, builds on the same four concepts:

1. **Transaction (Span/Slice).**
   A time-bounded event with a begin timestamp, an end timestamp, a name, and a parent context.
   In hardware verification tools the term is "transaction"; in distributed-systems observability the same idea is called a "span" (OpenTelemetry) or a "slice" (Perfetto).

2. **Stream (Track/Service).**
   A named timeline that groups related transactions.
   Streams correspond to transaction originators — the component where a transaction is born — so the viewer can display each originator's activity as a horizontal swim-lane.
   A CPU core's sequencer, for example, is a single stream; the routers and queues the request passes through are not separate streams but appear as event attributes on the transaction.

3. **Attributes (Properties/Annotations).**
   Key-value metadata attached to a transaction: address, request type, packet ID, latency breakdown, error status.
   Some formats distinguish *when* the attribute was captured (at transaction begin, during its lifetime, or at end).

4. **Relations (Links/Flows).**
   Explicit edges between transactions that express causality:
   "this L2 miss *caused* that network request," or "this flit *is a child of* that packet."
   Without relations, the viewer sees isolated bars on separate swim-lanes.
   With relations, it can draw arrows that turn a flat timeline into a navigable dependency graph.

### Transaction Traces in Practice

The concept appears under different names in several domains.
Comparing them shows how the same core model adapts to different environments.

**FSDB transaction traces (Synopsys Verdi).**
The industry-standard tool for hardware verification debug.
FSDB stores signal-level waveforms and transaction-level data in the same proprietary binary file.
UVM testbenches call `begin_tr` / `end_tr` during simulation; Verdi's nWave viewer then displays transactions as colored bars on stream swim-lanes.
Parent-child relationships are expressed by passing a parent transaction handle when beginning a child.
Attributes are typed key-value pairs added via the API.
FSDB is fast and mature but proprietary and closed-source, which makes it unsuitable for an open project like gem5.

**SystemC SCV (Accellera standard).**
The SystemC Verification library defines `scv_tr_stream`, `scv_tr_generator<T_begin, T_end>`, and `scv_tr_handle` for recording TLM-level transactions.
A generator is a typed factory that produces transactions on a stream.
Parent-child links are created by passing a parent handle to `begin_transaction()`.
SCV itself is only an API; the actual file output is pluggable.
Back-ends include plain text, SQLite, compressed text, and — most relevant here — the FTR binary format.
SCV established the stream/generator/transaction vocabulary that FTR inherits directly.

**OpenTelemetry traces (CNCF standard).**
The dominant open standard for distributed-systems observability.
A *trace* is a DAG of *spans* sharing a 128-bit trace ID.
Each span carries a span ID, a parent span ID, typed attributes, timestamped events, and *span links* that reference spans in other traces.
The wire format is Protocol Buffers (OTLP); viewers like Jaeger and Grafana Tempo render the span tree.
OpenTelemetry demonstrates that a small, well-defined data model — ID, parent ID, attributes, links — scales from a single process to planet-wide microservice deployments.

**Perfetto traces (Google, open-source).**
A high-performance system-wide tracing framework used in Android and Chrome.
Perfetto organizes data into *tracks* (swim-lanes identified by UUID with optional parent track) and *slices* (time-bounded events on a track).
Cross-track causality is expressed through *flow IDs*: a unique 64-bit ID attached to a begin event on one track and an end event on another, drawing an arrow in the UI.
The file format is binary protobuf with aggressive string interning and delta encoding.
Perfetto's track/flow model is a good conceptual match for hardware simulation — tracks map naturally to SimObjects, flows map to request forwarding — but Perfetto's protobuf schema is designed for software profiling, not hardware transaction recording.

### Why FTR?

FTR (Fast Transaction Recording) was created by MINRES Technologies as part of the SystemC-Components (SCC) project.
It is the modern high-performance back-end for SystemC SCV transaction recording, replacing the older text and SQLite back-ends.

FTR is a good fit for gem5 for several reasons:

- **Open source** (Apache 2.0) — no proprietary dependencies.
- **Hardware-oriented data model** — streams map to transaction originators, generators distinguish transaction kinds within an originator, and events carry SimObject names of the components a transaction passes through.
- **Compact binary format** — CBOR encoding with LZ4 compression and string interning keeps files small even for long simulations.
- **Explicit relations** — named directed edges between transactions, within a stream or across streams, exactly what is needed to link parent requests to child flits and, in the future, instructions to their memory requests.
- **Streaming writes** — transactions are flushed in chunks during simulation, so the writer does not accumulate unbounded state.
- **Viewable in SCViewer** — an open-source transaction viewer that understands the FTR format.

### FTR Data Model

FTR organizes trace data into five kinds of records, written as chunks in a CBOR-encoded binary file:

```
┌─────────────────────────────────────────────────────┐
│                   FTR File Layout                    │
├──────────┬──────────────────────────────────────────┤
│ Info     │ Timescale (e.g. 10⁻¹² = picoseconds),   │
│          │ creation timestamp                        │
├──────────┼──────────────────────────────────────────┤
│ Dictionary│ Interned string table (id → string).    │
│          │ Attribute names, stream names, relation   │
│          │ types are stored once, referenced by ID.  │
├──────────┼──────────────────────────────────────────┤
│ Directory│ Stream definitions (id, name, kind) and  │
│          │ generator definitions (id, name, stream). │
├──────────┼──────────────────────────────────────────┤
│ TX Blocks│ Batches of transactions for one stream,  │
│          │ with time-range metadata.  Each tx has:  │
│          │   id, generator, start_time, end_time,   │
│          │   and typed attributes.                   │
├──────────┼──────────────────────────────────────────┤
│ Relations│ Named directed edges:                    │
│          │   (name, src_stream, src_tx,             │
│          │    sink_stream, sink_tx)                  │
└──────────┴──────────────────────────────────────────┘
```

**Streams and generators.**
A *stream* is a named timeline — analogous to a Perfetto track or an OpenTelemetry service.
In gem5 terms, a stream maps to a transaction originator: the component where transactions are born.
For memory requests entering Ruby, the originator is the sequencer, so the stream is named after it (e.g. `system.ruby.l1_cntrl0.sequencer`).
A *generator* is a named source of transactions within a stream.
One sequencer stream has a `memreq` generator for root memory-request transactions and a `flit` generator for the flit child transactions created when those requests are flitisized into Garnet.
The intermediate components a transaction passes through — controllers, message buffers, routers, links — do not own separate streams; they appear as event attributes (object name, timestamp) stamped onto transactions living on the originator's stream.

**Transactions.**
A transaction is a time-bounded event on a generator's stream.
It has a unique 64-bit ID, a start time, an end time, and zero or more *attributes*.
Each attribute is a triple of (name, data type, value) tagged with an *event type* that says when the attribute was captured:

| Event Type | CBOR Tag | Meaning |
|------------|----------|---------|
| `BEGIN`    | 7        | Captured when the transaction starts (e.g. address, request type) |
| `RECORD`   | 8        | Captured during the transaction's lifetime (e.g. intermediate state) |
| `END`      | 9        | Captured when the transaction ends (e.g. final latency, completion status) |

Supported data types include `BOOLEAN`, `INTEGER`, `UNSIGNED`, `FLOATING_POINT_NUMBER`, `STRING`, `ENUMERATION`, `BIT_VECTOR`, `LOGIC_VECTOR`, `TIME`, and `POINTER`.

**Relations.**
A relation is a named directed edge from one transaction to another, possibly across different streams.
The name describes the relationship type — for example, `"parent_of"`, `"caused_by"`, or `"split_into"`.
Relations are how a viewer draws arrows from a root memory-request transaction to its flit child transactions (within the same originator stream), or from an instruction transaction on the CPU fetch stream to its memory-request child on the sequencer stream.

### FTR Writer API Overview

The C++ API (`ext/ftr/src/ftr/ftr_writer.h`) centers on the `ftr::ftr_writer<COMPRESSED>` class template.
The template parameter controls whether chunks are LZ4-compressed.

```cpp
ftr::ftr_writer<true> trace("output.ftr");  // compressed output

// 1. Metadata: set timescale to picoseconds
trace.writeInfo(-12);   // exponent: 10^-12

// 2. Directory: define stream and generators for one core's sequencer
trace.writeStream(/*id=*/0, "system.ruby.l1_cntrl0.sequencer", "Sequencer");
trace.writeGenerator(/*id=*/0, "memreq", /*stream=*/0);  // memory requests
trace.writeGenerator(/*id=*/1, "flit",   /*stream=*/0);  // flit children

// 3. Transactions: record a cache access (root memory request)
uint64_t tx = 1;
trace.startTransaction(tx, /*generator=*/0, /*stream=*/0, /*time=*/1000);

trace.writeAttribute(tx, ftr::event_type::BEGIN,
    "address", ftr::data_type::UNSIGNED, uint64_t(0x80001000));
trace.writeAttribute(tx, ftr::event_type::BEGIN,
    "type", ftr::data_type::STRING, "LD");
trace.writeAttribute(tx, ftr::event_type::RECORD,
    "object", ftr::data_type::STRING, "system.ruby.l1_cntrl0.mandatoryQueue");
trace.writeAttribute(tx, ftr::event_type::END,
    "latency", ftr::data_type::UNSIGNED, uint64_t(45));

trace.endTransaction(tx, /*time=*/1045);

// 4. Record a flit child on the same stream, different generator
uint64_t flit_tx = 2;
trace.startTransaction(flit_tx, /*generator=*/1, /*stream=*/0, /*time=*/1010);
trace.endTransaction(flit_tx, /*time=*/1040);

// 5. Relations: link parent request to child flit (same stream)
trace.writeRelation("parent_of",
    /*sink_stream=*/0, /*sink_tx=*/2,
    /*src_stream=*/0,  /*src_tx=*/1);
```

The key API methods are:

| Method | Purpose |
|--------|---------|
| `writeInfo(timescale)` | Set the time unit exponent and record creation time |
| `writeStream(id, name, kind)` | Define a named stream (maps to a transaction originator) |
| `writeGenerator(id, name, stream)` | Define a transaction source within a stream |
| `startTransaction(id, generator, stream, time)` | Begin a new transaction |
| `writeAttribute(id, event, name, type, value)` | Attach a typed attribute to a live transaction |
| `endTransaction(id, time)` | Complete a transaction |
| `writeRelation(name, sink_stream, sink_tx, src_stream, src_tx)` | Record a directed relationship between two transactions |

The writer handles string interning, chunk batching, and optional LZ4 compression internally.
Transaction entries are pooled and reused to minimize allocation overhead during simulation.
The destructor automatically flushes any remaining in-flight transactions and closes the file.

### How This Applies to gem5

The plan that follows designs a gem5-native tracing subsystem that uses FTR as its output format.
The mapping from FTR concepts to gem5 concepts is:

| FTR Concept | gem5 Mapping |
|-------------|-------------|
| Stream | Transaction originator (sequencer for memory requests, CPU fetch stage for instructions) |
| Generator | A transaction kind within an originator (`memreq`, `flit`, future `instruction`) |
| Transaction | The lifetime of a request, message, packet, or flit within one component |
| Relation | Parent-child or causal link between transactions (e.g. request → flit in v1, later request → message → flit) |
| Attribute | Address, request type, size, requestor ID, latency, state transitions |

A memory request entering Ruby will produce a single root transaction on the sequencer stream (e.g. `system.ruby.l1_cntrl0.sequencer`) under the `memreq` generator.
That transaction lives until the request retires.
In the first version, Garnet child transactions are created only at the flit level under the `flit` generator on the same sequencer stream.
Each flit gets its own child transaction because it may take a different path through the network and accumulate different per-hop timing.
Relations link child flit transactions directly back to their parent request within the same stream, so a viewer can follow the fan-out through the network and the eventual convergence back at the requestor.
Distinct `RubyMessage` and `NetworkPacket` transaction nodes are deferred to a later phase, when the framework can represent them with their own stable identities instead of overloading one message-local ID.

---

## Problem Statement

gem5 has strong statistics, debug printing, and some specialized trace outputs, but it does not have one coherent transaction trace that follows a memory request from the CPU-facing Ruby entry point through controllers, queues, network messages, flits, and final retirement.

For Ruby work, that missing continuity makes it hard to answer the questions that matter during model development.

Where did this request stall?

Which SimObject owned it at each point in time?

When did one logical request become multiple network packets or flits?

How do we correlate a CPU-visible miss with the exact Garnet path that carried it?

The immediate goal is to trace the memory subsystem, starting at CPU requests entering Ruby and ending when the original request retires.

The longer-term goal is broader.

The same design should later support tracing instruction lifetimes in an O3 pipeline without inventing a second framework.

That future requirement strongly affects the architecture.

The trace must not be Ruby-only in its core data model.

It must be a generic transaction framework whose first major client is Ruby.

## Goals

1. Create one top-level FTR transaction for every CPU request accepted into Ruby.
2. Keep that transaction alive until the request retires back toward the CPU.
3. Create child transactions when a Ruby message is split into Garnet flits.
4. Record timestamps when the transaction hits important objects in the design hierarchy.
5. Record stable attributes such as address, request type, size, requestor, and related identifiers.
6. Use dotted hierarchical names that match the canonical SimObject hierarchy.
7. Preserve parent and child relationships across request, message, packet, and flit lifetimes.
8. Keep the design efficient enough to be usable on real Ruby experiments.
9. Keep the core architecture generic enough to trace future O3 instruction transactions.

## Non-Goals For The First Version

1. Capturing every possible internal micro-event in Ruby.
2. Solving all protocol-specific message correlation in one first patch.
3. Building a universal viewer.
4. Replacing existing statistics or debug flags.
5. Recording every value as a waveform.

FTR should be event and transaction oriented, not a second stats system and not a clone of FST.

## Design Principles

1. Put the durable transaction identity at the earliest common abstraction that survives subsystem growth.
2. Treat SimObject names as the source of truth for hierarchy naming.
3. Separate trace transport from trace semantics.
4. Prefer a small number of stable event kinds over many ad hoc log records.
5. Allow partial instrumentation so Ruby can start useful before every controller and protocol is annotated.
6. Keep parent and child relationships explicit instead of reconstructing them offline.
7. Avoid tying the design to one coherence protocol or one network implementation detail.

## Core Insight

The best anchor for a future-proof transaction trace is not `RubyRequest`, not `Message`, and not `flit`.

It is the memory `Request` plus a generic trace context carried alongside subsystem-specific objects.

`Request` already contains useful cross-cutting identity such as requestor ID, context ID, PC, task ID, stream ID, substream ID, and instruction sequence number.

`Request` is also `Extensible<Request>`, which is the cleanest place to attach durable trace metadata without baking tracing into every consumer.

Ruby and Garnet then become producers of additional transaction nodes and timestamped events under that top-level identity.

## Recommended Software Architecture

### Overview

Introduce a generic tracing subsystem with three layers.

1. `TraceContext`: small metadata attached to long-lived origin objects such as `Request`.
2. `TransactionTraceRecorder`: a runtime service that allocates IDs, records events, manages retirement, and writes FTR output.
3. Subsystem adapters: Ruby-first instrumentation that creates and updates transactions as requests move through Ruby and Garnet.

The recorder should be implemented as a `SimObject` so it cleanly follows gem5 lifecycle rules, output path handling, drain, checkpoint interactions, and final flush.

The first concrete instance can be named `FtrTrace`.

### Main Components

#### 1. `FtrTrace` SimObject

`FtrTrace` owns the output file, ID allocator, configuration, event buffering, and final serialization.

It should initialize early, register listeners during `regProbeListeners()`, and flush on exit.

Like the FST plan, it should rely on gem5 lifecycle ordering instead of ad hoc startup code.

#### 2. `TraceContext`

`TraceContext` is the durable origin metadata attached to a `Request` via `Extensible<Request>`.

It should carry only trace-specific identity fields that do not already exist on `Request`.

`Request` already contains `requestorId`, `contextId`, `taskId`, `streamId`, `substreamId`, `_pc`, and `_instSeqNum`.

Duplicating those into `TraceContext` wastes space and creates a consistency hazard.

Recommended `TraceContext` fields are:

- `trace_id` — globally unique ID for this transaction node
- `root_trace_id` — ID of the root ancestor (equals `trace_id` for root nodes)
- `parent_trace_id` — ID of the parent node (zero for root nodes)
- `origin_tick` — tick when the root transaction was created

Domain-specific attributes such as address, requestor ID, PC, and instruction sequence number should be read directly from `Request` when the recorder needs them, not copied into the extension.

For memory requests, `trace_id == root_trace_id` at creation.

For future O3 instruction tracing, the instruction transaction would become the root, and the memory request could then become a child of the instruction transaction.

That is why the model must support `root_trace_id` and `parent_trace_id` from day one.

#### 2a. Trace Identity On `Message` And `flit`

`TraceContext` lives on `Request`, but Ruby's internal `Message` class does not carry a `Request*`.

Only `RubyRequest` (a `Message` subclass) has `m_pkt` pointing back to the original packet and request.

Protocol response messages, forwarded requests, and SLICC-generated messages do not.

This creates a bridging gap: when `MessageBuffer::enqueue()` receives a `MsgPtr`, it cannot generically reach the `TraceContext` on the originating `Request`.

The recommended solution is to add a single root-trace field to the `Message` base class.

```cpp
class Message
{
    // ... existing fields ...
    TraceId m_traceId = 0;  // 0 means untraced; root request trace ID only
};
```

In the first version, this field always means "which root request tree does this message belong to?"
It does not identify a distinct `RubyMessage` or `NetworkPacket` transaction node.

This field is set once when trace identity is first associated with the message:

1. For `RubyRequest`, set `m_traceId` from the `TraceContext` on `m_pkt->req` at creation time.
2. For protocol messages derived from a traced request, copy `m_traceId` from the originating message during SLICC action code or controller logic.
3. For messages with no traced origin, leave it zero.

The `flit` class gets its own distinct transaction-node ID:

```cpp
class flit
{
    // ... existing fields ...
    TraceId m_traceId = 0;  // unique flit node ID
};
```

Set it during `flitisizeMessage()` by allocating a new flit child transaction under the root request ID carried by `msg_ptr->m_traceId`.

This gives `MessageBuffer::enqueue/dequeue()` a cheap way to find the root transaction, and gives Garnet router stages a stable, flit-local node ID that remains constant as the flit traverses multiple routers and links.

The cost is one 64-bit field per `Message` and one per `flit`, which is negligible relative to existing object sizes.

#### 2b. Recorder Access Pattern

Framework primitives (`TimingRequestProtocol`, `MessageBuffer`, Garnet stages) need a way to find the `FtrTrace` recorder without passing it through every call site.

The recommended approach is a process-global singleton pointer.

```cpp
class FtrTrace : public SimObject
{
    static FtrTrace *instance;  // set during construction, cleared on destruction
  public:
    static FtrTrace *get() { return instance; }
};
```

All primitive-level hooks check `FtrTrace::get()` first.

If null, tracing is disabled and the hook is a single branch on a null pointer — effectively zero cost.

This is the same pattern used by `Debug::SimpleFlag::tracing` for DPRINTF and by gem5's `Trace::output()` for trace output.

For multi-threaded simulation with multiple event queues, the singleton is safe because:

1. `FtrTrace` is constructed during Python-side `m5.instantiate()`, which runs single-threaded before simulation begins.
2. The pointer is read-only during simulation.
3. The ID allocator inside `FtrTrace` should use `std::atomic<uint64_t>` for thread-safe monotonic allocation across event queues.

#### 3. `TransactionNode`

The recorder should internally manage transaction nodes rather than a flat event stream only.

Recommended node kinds are:

1. `MemoryRequest`
2. `Flit`
3. Future: `RubyMessage`, `NetworkPacket`, `Instruction`, `PipelineStageToken`, `DMARequest`, `Interrupt`

Each node has:

- globally unique `trace_id` for this exact node
- `root_trace_id` naming the root request tree this node belongs to
- optional `parent_trace_id` naming the immediate parent node
- `kind`
- `begin_tick`
- optional `end_tick`
- `creation_object`
- `retirement_object`
- static attributes map
- status

In the first version, a root `MemoryRequest` node has `trace_id == root_trace_id` and `parent_trace_id == 0`.
A `Flit` node has its own unique `trace_id`, the root request's ID as `root_trace_id`, and the root request's ID again as `parent_trace_id` because flits attach directly to the root in v1.

Static attributes are values that should not change after node creation, such as address, size, request type, vnet, packet ID, flit index, source router, and destination router.

#### 4. `TransactionEvent`

A transaction node alone is not enough.

We also need timestamped events attached to a node.

Recommended event payload fields are:

- `tick`
- `trace_id`
- `event_kind`
- `object_name`
- optional `stage_name`
- optional key-value attributes

Examples of `event_kind` are:

1. `Created`
2. `Accepted`
3. `Enqueue`
4. `Dequeue`
5. `Issued`
6. `Split`
7. `Inject`
8. `Arrive`
9. `RouteCompute`
10. `SwitchAlloc`
11. `SwitchTraverse`
12. `LinkTraverse`
13. `Reassemble`
14. `Callback`
15. `Retire`
16. `Destroy`

This event vocabulary is intentionally generic so O3 can later reuse it.

### Why Separate Nodes And Events

The FTR trace needs both structure and timing.

Nodes answer, “what logical thing is this?”

Events answer, “what happened to it, when, and where?”

That split keeps the format efficient and future proof.

It also avoids stuffing changing timestamps into a monolithic object dump record.

## Naming And Hierarchy Policy

All dotted hierarchy names written into FTR must come from canonical gem5 object names.

For SimObjects, the source of truth is `SimObject::name()` in C++ and `path()` in Python.

No custom naming scheme should be invented for traced hardware objects.

If a transaction event occurs in `system.ruby.l1_cntrl0.mandatoryQueue`, that exact dotted name should appear in the trace.

For sub-object entities that are not SimObjects, use a SimObject owner plus a local stage or component name.

Examples are:

- object: `system.ruby.network.netifs0`
- stage: `flitisize`

- object: `system.ruby.network.routers3`
- stage: `switch_allocator`

This keeps the primary hierarchy stable and viewer-friendly while still distinguishing internal pipeline stages.

## Stream Hierarchy In A Typical Configuration

Streams correspond to transaction originators — the component where a transaction is born — not to every SimObject the transaction passes through.
Intermediate components (controllers, message buffers, routers, links) appear as `object_name` attributes on events stamped onto transactions, but do not own separate streams.

This means the viewer shows one swim-lane per originator, and a request's entire life — queue hops, router traversals, callbacks — appears as events within a single transaction bar on that lane.

### Memory Request Streams

For a 4-core MESI Two Level system with Garnet, there are four memory-request streams, one per sequencer:

| Stream name | Kind | Generators |
|-------------|------|------------|
| `system.ruby.l1_cntrl0.sequencer` | `Sequencer` | `memreq`, `flit` |
| `system.ruby.l1_cntrl1.sequencer` | `Sequencer` | `memreq`, `flit` |
| `system.ruby.l1_cntrl2.sequencer` | `Sequencer` | `memreq`, `flit` |
| `system.ruby.l1_cntrl3.sequencer` | `Sequencer` | `memreq`, `flit` |

The `memreq` generator produces root `MemoryRequest` transactions — one per CPU request accepted into Ruby.
The `flit` generator produces `Flit` child transactions — one per flit created when a message originating from this core's request is flitisized into Garnet.

Both generators live on the same stream so that a request and its flit children appear together on the same viewer swim-lane, making it easy to see the full lifetime of one core's traffic.

Events on these transactions carry the `object_name` of the component where they occur.
For example, a single `MemoryRequest` transaction on `system.ruby.l1_cntrl0.sequencer` might accumulate events with `object_name` values such as:

- `system.ruby.l1_cntrl0.mandatoryQueue` (enqueue)
- `system.ruby.l2_cntrl0.L1RequestToL2Cache` (enqueue at L2)
- `system.ruby.network.routers0` (router arrive, for a flit child)
- `system.ruby.network.int_links01.network_link` (link traverse, for a flit child)
- `system.ruby.dir_cntrl0.requestToDir` (enqueue at directory)

These intermediate SimObject names appear as event attributes, not as separate streams.

### Future Instruction Streams

Instruction lifetime transactions originate in the CPU pipeline, not at the sequencer.
They belong on a separate stream at the fetch or decode stage:

| Stream name | Kind | Generators |
|-------------|------|------------|
| `system.cpu0.fetch` | `Fetch` | `instruction` |
| `system.cpu1.fetch` | `Fetch` | `instruction` |
| ... | | |

An instruction transaction on `system.cpu0.fetch` would parent a memory-request transaction on `system.ruby.l1_cntrl0.sequencer` via an explicit FTR relation.
The two transactions live on different streams because they have different originators, but the relation arrow connects them in the viewer.

### CHI Protocol Variant

For CHI-based configurations, the same stream-per-originator pattern applies.
The stream is named after the RNF's sequencer:

| Stream name | Kind | Generators |
|-------------|------|------------|
| `system.ruby.hnf00.cntrl.sequencer` | `Sequencer` | `memreq`, `flit` |

### What Does Not Get Its Own Stream

The following components appear only as event attributes, never as streams:

- Cache controllers (`system.ruby.l1_cntrl0`, `system.ruby.l2_cntrl0`, `system.ruby.dir_cntrl0`)
- Message buffers (`system.ruby.l1_cntrl0.mandatoryQueue`, `system.ruby.l2_cntrl0.responseToL2Cache`, etc.)
- Network interfaces (`system.ruby.network.netifs00`)
- Routers (`system.ruby.network.routers00`)
- Links (`system.ruby.network.int_links00.network_link`)
- Memory controllers (`system.mem_ctrls0`)

All of these stamp events onto existing transactions — they do not originate transactions.

## ID Model

Use one global 64-bit monotonic ID space for all transaction nodes.

Do not use separate ID spaces for requests, messages, and flits.

Separate spaces make future cross-subsystem joins harder and create unnecessary complexity.

Use explicit `kind` fields instead.

Recommended correlation rules are:

1. CPU request entering Ruby creates root `MemoryRequest` node.
2. If the NI injects a traced message into Garnet, create a child `Flit` node per constructed flit.
3. Distinct `RubyMessage` and `NetworkPacket` nodes are optional later refinements, not required for the first implementation.
4. Retirement of a parent does not automatically imply retirement of live children.
5. All children retain the same `root_trace_id`.

This structure allows offline tools to answer both “show me the whole request tree” and “show me only network flits.”

## Ruby-First Integration Strategy

### Phase 1 Scope

The first implementation should cover the common end-to-end path for CPU-originated Ruby requests.

That path is:

1. CPU packet enters `RubyPort`.
2. `Sequencer` accepts and tracks the request.
3. `RubyRequest` is created and issued to the controller.
4. Controllers and `MessageBuffer`s enqueue and dequeue messages.
5. `NetworkInterface` flitisizes network traffic.
6. Garnet routers and links move flits.
7. Response returns to the controller and `Sequencer` callback.
8. `RubyPort` sends completion back toward the CPU.

### Root Transaction Creation

Root transaction creation is handled by the port-level primitive instrumentation (Step 3 in the implementation plan), not by manual hooks in `RubyPort`.

The port-level hook in `TimingRequestProtocol::sendReq()` fires on every timing request.

However, not every port crossing should create a root transaction.

The hook must distinguish "this request is entering a traced subsystem for the first time" from "this is an internal port crossing within a subsystem that is already being traced."

The recommended detection rule is:

1. Check whether `pkt->req` already has a `TraceContext` extension.
2. If it does not, and the recorder is configured to trace this request (based on filtering criteria such as requestor ID, address range, or the receiving port's owner), create a root `MemoryRequest` transaction and attach the `TraceContext` extension to `pkt->req`.
3. If it already has a `TraceContext`, stamp a `PortCrossing` event on the existing transaction.

This avoids the need for the port protocol to know whether it is entering Ruby specifically.

The filtering configuration on `FtrTrace` determines which subsystem boundaries trigger root creation.

For the first implementation, a simple policy is sufficient: create a root transaction for any request that does not already have a `TraceContext` and whose target `ResponsePort` owner is a `RubyPort`.

The transaction should only be committed as live once the request is accepted rather than merely observed.

If a retry path causes the request not to enter Ruby yet, the trace must not leak a fake live transaction.

The practical rule is:

1. The port-level hook creates the `TraceContext` extension on `pkt->req` with a pending state.
2. `Sequencer::makeRequest()` finalizes the root transaction as live (this is one of the three manual hooks in Step 4).
3. If Ruby rejects the request for retry, the pending `TraceContext` is removed or left inert, and no committed live transaction exists in the recorder.

### Recommended Anchor For Durable Metadata

Attach `TraceContext` to `pkt->req`, not to `Packet`, `RubyRequest`, or `SequencerRequest`.

`Packet` is too transport-local.

`RubyRequest` does not survive the full Ruby plus network lifetime.

`SequencerRequest` is useful for outstanding bookkeeping but is Ruby-specific.

`Request` is the right durability layer.

### Outstanding Request Tracking

The recorder maintains an internal live-node table keyed by `trace_id`.

The `TraceContext` on `Request` and the `m_traceId` on `Message` and `flit` all reference entries in this table.

No secondary indexing by `Request*` or `MsgPtr` is needed because the `trace_id` is always directly available on the object being processed.

For sequencer-side bookkeeping, it is reasonable to cache `trace_id` in `SequencerRequest` too, but that should be a convenience shortcut, not the single source of truth.

## Message And Flit Child Transactions

### Ruby Message Nodes

Not every internal message must become a transaction node in the first patch.

But the design should support it cleanly.

The recommended policy is:

1. Root node is always `MemoryRequest`.
2. Internal and protocol-visible queue motion is emitted on the root request node in the first patch.
3. Distinct `RubyMessage` child nodes may be added later when the framework can assign a stable message-local transaction ID that is not overloaded with root identity.

This policy avoids requiring a full protocol-wide SLICC metadata retrofit on day one.

### Flit Nodes

In `NetworkInterface::flitisizeMessage()`, create a `Flit` child node for each constructed flit.
The flit transaction is created on the same sequencer stream as its parent root request, under the `flit` generator.
The originator's stream is determined by looking up the root request's `root_trace_id` from `msg_ptr->m_traceId`.

In the first version, flits attach directly to the root `MemoryRequest` node carried by `msg_ptr->m_traceId`.
This avoids inventing an ambiguous message-local ID that would have to represent both the root request and multiple packet or flit descendants during multicast fan-out.

Each flit node should carry:

- parent root request trace ID
- flit index
- Garnet packet ID
- flit type
- width
- vnet
- VC if known
- route metadata if available

Child creation must be explicit in the trace rather than inferred offline from packet size and link width.

That explicitness matters for correctness once multicast, bridges, or SerDes paths enter the picture.

If later phases add true `NetworkPacket` nodes with stable packet-local IDs, the parent of each flit can change from the root request to the packet node without changing the recorder API.

## Timestamp Capture Model

### General Rule

Record timestamps as events at subsystem boundaries and meaningful internal stages.

Do not try to stamp every line of code.

Choose hook points that correspond to ownership changes, resource contention points, and visible state transitions.

### Required Ruby Timestamps

For the first version, record at least these timestamps on the root `MemoryRequest` node.

Automatic timestamps (captured by primitive-level hooks, no manual code needed):

1. `PortCrossing` — every port boundary the request crosses, including entry into Ruby and exit back toward the CPU (port-level hook)
2. `Enqueue` / `Dequeue` — every `MessageBuffer` operation the associated message passes through (`MessageBuffer` hook)

Manual timestamps (captured by the three Sequencer hooks):

3. `RubyIssued` — when `Sequencer::issueRequest()` hands the request to the protocol controller
4. `SequencerCallback` — when `readCallback()`, `writeCallback()`, or `atomicCallback()` fires with hit/miss information

The automatic port-crossing stamps subsume the old manual `RubyAccepted`, `RubyResponseSent`, and `Retired` events because entry and exit port crossings are captured by the port-level primitive.

### Required Queue Timestamps

Captured automatically by the `MessageBuffer` primitive hook.

For every `enqueue()` and `dequeue()` call on any `MessageBuffer` in the system:

1. Look up `m_traceId` on the message being queued.
2. If non-zero, stamp an `Enqueue` or `Dequeue` event on that trace node.
3. Include the buffer's `SimObject::name()`, `curTick()`, queue occupancy, and vnet.

No per-protocol and no per-controller instrumentation is needed.

### Required Garnet Timestamps

For `Flit` nodes, record at least:

Automatic timestamps (captured by `NetworkInterface` instrumentation and router ProbePoints):

1. `Created` at flit construction in `flitisizeMessage()`
2. `Inject` when scheduled onto the outgoing NI path
3. `RouterArrive` at `InputUnit::wakeup()` (ProbePoint)
4. `SwitchAlloc` at `SwitchAllocator` (ProbePoint)
5. `SwitchTraverse` at `CrossbarSwitch` (ProbePoint)
6. `LinkTraverse` at `NetworkLink::wakeup()` (ProbePoint)
7. `Eject` at destination NI arrival
8. `Destroy` when the flit retires from NI processing

All of these are captured by the four framework-level changes (NI instrumentation + four ProbePoints).

These timestamps align with Garnet's real pipeline stages and give enough fidelity to explain contention and routing delay.

### SimpleNetwork Considerations

SimpleNetwork does not have flits, routers, or a `flitisizeMessage()` funnel.

It moves messages directly between `MessageBuffer` endpoints through `SimpleNetwork::makeLink()` paths.

For SimpleNetwork, the `MessageBuffer` enqueue/dequeue hooks provide automatic queue-level tracing.

No packet or flit child transactions are created because SimpleNetwork does not model packet-level decomposition.

This is acceptable for the first version because SimpleNetwork is a functional abstraction, not a timing-accurate network model.

If finer-grain SimpleNetwork tracing is needed later, add ProbePoints to `PerfectSwitch` and `Throttle`.

## Instrumentation Mechanisms

### Core Principle: Instrument Primitives, Not Components

The most important architectural decision is to embed tracing into the small number of framework primitives that all traffic already flows through, rather than manually instrumenting each component.

gem5 has a handful of narrow funnels through which all memory transactions pass.

By instrumenting these funnels once, every current and future component that uses them gets traced automatically with zero per-component work.

The funnels are:

1. `TimingRequestProtocol::sendReq()` and `TimingResponseProtocol::sendResp()` for port crossings.
2. `MessageBuffer::enqueue()` and `MessageBuffer::dequeue()` for Ruby queue motion.
3. `NetworkInterface::flitisizeMessage()` for Garnet flit creation.
4. Garnet router stage entry points for per-hop pipeline stamps.

Manual instrumentation should be reserved only for domain-specific attribute capture that the primitives cannot know about, such as recording protocol-specific fields at root transaction creation.

### Leverage The Existing Port-Level Tracing Pattern

gem5 already has a `TracingExtension` on `Packet` (defined in `src/mem/port.hh`) and `addTrace()` / `removeTrace()` hooks called automatically around every `sendTimingReq()`, `sendTimingResp()`, and `sendTimingSnoopReq()` call in `RequestPort`.

The current implementation only pushes port name strings onto a stack for debug printing.

This is the exact pattern FTR needs, and it already executes on every timing transaction in the entire system.

The recommended approach is to generalize this existing mechanism.

In `TimingRequestProtocol::sendReq()` and `TimingResponseProtocol::sendResp()`, check whether the packet's `Request` carries a `TraceContext` extension.

If it does, notify the FTR recorder of the port crossing with the peer's `SimObject::name()` and `curTick()`.

This is approximately five lines of code in two methods that all timing requests and responses already pass through.

The benefits are:

1. Every port crossing in the entire system (Classic caches, Ruby, crossbars, future DMA, future O3 LSQ) is traced automatically.
2. Root transaction creation can be detected as the first port crossing into a Ruby `ResponsePort`.
3. Response retirement can be detected as the response crossing back out through the originating port.
4. No per-component instrumentation is needed for port-crossing timestamps.
5. New components added in the future automatically get traced if they use the standard port protocol.

This single change replaces the need for manual hooks in `RubyPort::MemResponsePort::recvTimingReq()`, `RubyPort::ruby_hit_callback()`, `RubyPort::MemResponsePort::hitCallback()`, and similar response-path methods.

### Instrument MessageBuffer Once, Cover All Protocols

`MessageBuffer` is a single concrete class (not a base class with many overrides).

Every Ruby protocol message in every coherence protocol passes through its `enqueue()` and `dequeue()` methods.

`MessageBuffer` already has a `registerDequeueCallback()` mechanism (used by controllers to wake up on message arrival), which demonstrates that callback-style hooks are an accepted pattern for this class.

The recommended approach is to make `MessageBuffer` trace-aware in its base implementation.

The hook reads `m_traceId` from the `Message` being queued (see section 2a above).

If `m_traceId` is non-zero and `FtrTrace::get()` is non-null, emit a trace event.

No type-testing or pointer chasing is needed — the `m_traceId` field is on the `Message` base class.

```cpp
void MessageBuffer::enqueue(MsgPtr message, ...)
{
    // ... existing logic ...
    if (auto *recorder = FtrTrace::get();
        recorder && message->m_traceId != 0) {
        recorder->stampEvent(message->m_traceId, "Enqueue",
                             name(), curTick(),
                             {{"occupancy", m_prio_heap.size()},
                              {"vnet", message->getVnet()}});
    }
    // ... existing logic ...
}
```

The event payload includes:

1. The `MessageBuffer`'s `SimObject::name()` as the object name.
2. `curTick()` as the timestamp.
3. Queue occupancy at the time of the event.
4. The vnet of the message.

This covers all queue timestamps for all protocols and all controllers with one code change.

No per-protocol and no per-controller instrumentation is needed for queue events.

Messages with `m_traceId == 0` (untraced messages, such as protocol-internal bookkeeping) are skipped at zero cost.

### Use ProbePoints For Garnet Router Stages

The existing probe framework (`src/sim/probe/probe.hh`) supports typed listeners, clean lifecycle registration via `regProbeListeners()`, and zero overhead when no listener is registered (the `ProbePointArg::notify()` check is a simple pointer test).

Rather than adding direct FTR recorder calls into each Garnet router stage, add one `ProbePointArg<FlitTraceStamp>` to each of the four router pipeline stages.

The stages and their probe points are:

1. `InputUnit::wakeup()` — `probeRouterArrive`
2. `SwitchAllocator::wakeup()` — `probeSwitchAlloc`
3. `CrossbarSwitch::wakeup()` — `probeSwitchTraverse`
4. `NetworkLink::wakeup()` — `probeLinkTraverse`

The `FlitTraceStamp` struct should carry:

1. Flit trace ID (from the flit's trace metadata).
2. `SimObject::name()` of the router or link.
3. `curTick()`.
4. VC and outport where applicable.

The `FtrTrace` SimObject registers as a `ProbeListener` for these probe points during `regProbeListeners()`.

If `FtrTrace` is not instantiated in the simulation, the probes fire to zero listeners and the cost is negligible.

The benefits are:

1. Router tracing is zero-cost when FTR is not configured.
2. Custom router implementations automatically get tracing if they fire the same probes.
3. No coupling between router code and FTR serialization format.
4. The probe framework handles listener lifecycle and cleanup.

### What Remains Manual

Two categories of instrumentation cannot be fully automated through primitives.

1. **Domain-specific attribute capture and lifecycle events at the Sequencer.** Recording request attributes (address, type, size, flags), finalizing the pending root transaction, stamping `RubyIssued` and `SequencerCallback` events, propagating `m_traceId` from `Request` to `RubyRequest`, and closing the root transaction on retirement all require explicit code in the Sequencer (three hook points total).

2. **Protocol-specific message child nodes and trace ID propagation to newly constructed messages.** Creating `RubyMessage` child transactions for protocol messages that have semantic significance (such as upgrade requests or invalidations) requires understanding protocol semantics that the framework cannot infer. Propagating `m_traceId` to newly constructed (not cloned) protocol response messages requires SLICC action code changes per protocol.

Note that trace identity propagation through message *cloning* is automatic because `m_traceId` is a base `Message` field copied by `clone()`.

These manual points should be addressed in later phases after the primitive-level instrumentation is proven useful.

### Allow Direct Recorder Calls Where Primitives Are Insufficient

For the small number of manual instrumentation points described above, direct recorder calls are acceptable.

Wrap them behind tiny helper functions from day one.

Later, if the same semantic events prove useful to other listeners, convert those helpers to emit typed probes.

## Proposed Runtime API

The recorder should expose a small explicit API.

Example operations are:

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

The exact API can vary, but it should preserve these semantics.

Creation, stamping, and retirement must be first-class operations.

## Data Written To FTR

For every node, record:

1. IDs: `trace_id`, `root_trace_id`, `parent_trace_id`
2. kind
3. begin and end ticks
4. creation and retirement object names
5. static attributes

For root memory requests, static attributes are read from `Request` at root finalization time (Sequencer Step 4), not from `TraceContext`.

They should include at least:

1. physical address if valid
2. line address if known
3. request size
4. command or Ruby request type
5. requestor ID (`req->requestorId()`)
6. context ID (`req->contextId()`)
7. task ID (`req->taskId()`)
8. stream ID and substream ID if present
9. PC if present (`req->getPC()`)
10. instruction sequence number if present (`req->getInstSeqNum()`)
11. secure, prefetch, atomic, LLSC, HTM, and TLBI related flags where applicable

For flit nodes in the first version:

1. `trace_id` is the unique flit node ID stored on the `flit`
2. `root_trace_id` is the root request ID copied from `Message::m_traceId`
3. `parent_trace_id` equals the root request ID because flits attach directly to the root in v1
4. static attributes include flit index, Garnet packet ID, vnet, VC if known, width, source NI name, destination node, and route metadata if available

For events, record:

1. timestamp
2. event kind
3. owning object name
4. optional stage name
5. optional dynamic attributes such as queue occupancy, VC, outport, or hop count

## Output Format Guidance

The trace transport should be treated as a separate concern from the recorder.

The first implementation may write directly in the FTR format if the format is already fixed.

If the format is still evolving, define an internal schema first and keep the writer behind one narrow interface.

That avoids hard-wiring Ruby instrumentation to one serialization decision.

The trace file path should follow existing gem5 trace conventions and resolve under the output directory.

## Why This Is Better Than A Ruby-Only Side Table Design

A Ruby-only external side table keyed by `PacketPtr`, `MsgPtr`, and flit IDs can work for a prototype.

It is not the best long-term architecture.

It makes future O3 instruction tracing awkward.

It also pushes too much correlation burden into fragile pointer identity rules.

The recommended design still allows small side tables internally where needed, but the durable semantics live in `Request`-anchored trace context plus recorder-managed transaction nodes.

## Why Primitive-Level Instrumentation Is Better Than Per-Component Hooks

A manual approach that lists 20 or more individual hook points across RubyPort, Sequencer, MessageBuffer, NetworkInterface, InputUnit, SwitchAllocator, CrossbarSwitch, OutputUnit, and NetworkLink has several problems.

1. **It does not scale.** Every new component, protocol, or network variant requires adding more hooks.
2. **It is fragile.** Refactoring a component can silently drop trace coverage.
3. **It creates coupling.** Each instrumented component must know about the FTR recorder API.
4. **It front-loads too much work.** The first useful trace requires touching many files across multiple subsystems.

The primitive-level approach inverts this.

gem5 has a small number of framework primitives that all traffic flows through regardless of component or protocol.

By instrumenting these primitives once, tracing becomes an automatic property of the framework rather than an obligation of each component.

The key primitives are:

| Primitive | Location | What it covers |
|-----------|----------|---------------|
| `TimingRequestProtocol::sendReq/sendResp` | `src/mem/protocol/timing.cc` | Every timing port crossing system-wide |
| `MessageBuffer::enqueue/dequeue` | `src/mem/ruby/network/MessageBuffer.cc` | Every Ruby queue operation, all protocols |
| `NetworkInterface::flitisizeMessage` | `src/mem/ruby/network/garnet/NetworkInterface.cc` | Every Garnet flit creation |
| Garnet router stage ProbePoints | `InputUnit`, `SwitchAllocator`, `CrossbarSwitch`, `NetworkLink` | Every router pipeline stage |

The existing `addTrace()` / `removeTrace()` pattern in `RequestPort` (which already wraps every `sendTimingReq` call with a `TracingExtension` on `Packet`) demonstrates that gem5 already accepts this pattern for port-level concerns.

The `registerDequeueCallback()` mechanism on `MessageBuffer` demonstrates that callback-style hooks are accepted for queue-level concerns.

The `ProbePointArg<T>` framework demonstrates that zero-cost-when-unused event notification is accepted for stage-level concerns.

FTR tracing should follow these existing patterns rather than inventing a new per-component instrumentation discipline.

### Impact On Future Components

When a developer adds a new cache controller, coherence protocol, network topology, or CPU model:

- If it uses standard ports, it gets port-crossing traces automatically.
- If it uses `MessageBuffer`, it gets queue traces automatically.
- If it uses Garnet's `NetworkInterface`, its traffic gets flit traces automatically.
- If it uses Garnet routers, it gets router stage traces automatically.

No tracing code needs to be added to the new component.

Only domain-specific attribute capture (analogous to the three manual Sequencer hooks) would require new instrumentation.

## Recommended Hook Points

### Automatic: Port-Level Crossing (Framework Primitive)

Instrument once in the timing protocol base classes.

1. `TimingRequestProtocol::sendReq()` — stamps every timing request crossing any port boundary.
2. `TimingResponseProtocol::sendResp()` — stamps every timing response crossing any port boundary.

These two methods cover every port crossing in the entire system.

All Classic cache, Ruby, crossbar, bridge, DMA, and future O3 LSQ transactions are captured without per-component work.

Root transaction creation is detected as the first request crossing into a Ruby `ResponsePort`.

Response retirement is detected as the response crossing back through the originating port.

### Automatic: Queue Motion (Framework Primitive)

Instrument once in the `MessageBuffer` base implementation.

1. `MessageBuffer::enqueue()` — stamps every message entering any Ruby queue.
2. `MessageBuffer::dequeue()` — stamps every message leaving any Ruby queue.

These two methods cover all queue timestamps for all protocols and all controllers.

### Automatic: Garnet Injection (Framework Primitive)

Instrument once in `NetworkInterface`.

1. `NetworkInterface::flitisizeMessage()` — creates `Flit` child transactions for every message injected into Garnet.
2. Destination-side `NetworkInterface::wakeup()` — stamps ejection and reassembly.

### Automatic: Router Stages (ProbePoints)

Add one `ProbePointArg<FlitTraceStamp>` to each Garnet pipeline stage.

1. `InputUnit::wakeup()` — `probeRouterArrive`
2. `SwitchAllocator::wakeup()` — `probeSwitchAlloc`
3. `CrossbarSwitch::wakeup()` — `probeSwitchTraverse`
4. `NetworkLink::wakeup()` — `probeLinkTraverse`

The `FtrTrace` SimObject registers as a listener.

If `FtrTrace` is not instantiated, these probes fire to zero listeners at negligible cost.

### Manual: Domain-Specific Attribute Capture

These points require explicit instrumentation because they capture domain-specific attributes that the framework primitives cannot infer.

1. `Sequencer::makeRequest()` — attach `TraceContext` extension to `Request`, record address, type, size, requestor ID, PC, instruction sequence number.
2. `Sequencer::issueRequest()` — stamp `RubyIssued` event with protocol-specific issue details.
3. `Sequencer::readCallback()` / `writeCallback()` / `atomicCallback()` — stamp `SequencerCallback` event with hit/miss type and data source.

These are approximately three manual hook points compared to the nine in a purely manual approach.

### Summary: Hook Point Count

| Category | Hook points | Coverage |
|----------|-------------|----------|
| Port crossings (automatic) | 2 | All timing transactions system-wide |
| Queue motion (automatic) | 2 | All Ruby message buffers, all protocols |
| Garnet injection (automatic) | 2 | All packet/flit creation and ejection |
| Router stages (ProbePoints) | 4 | All Garnet router pipeline stages |
| Domain-specific (manual) | 3 | Request attributes and sequencer semantics |
| **Total** | **13** | **End-to-end request-to-flit tracing** |

Of the 13 hook points, 10 are framework-level changes that require no per-component work and automatically cover future components.

Only 3 are manual, domain-specific hooks in the Sequencer.

## Ruby Protocol Considerations

The design must work even before every SLICC protocol message is extended with explicit trace metadata.

### Trace Identity Propagation Through Protocol Messages

The `m_traceId` field on `Message` (added in Step 2b) provides the propagation mechanism.

For the first version, trace identity flows automatically through two paths:

1. **RubyRequest creation.** `Sequencer::makeRequest()` copies `trace_id` from the `TraceContext` on `pkt->req` into the `RubyRequest`'s `m_traceId`. This happens in one of the three manual hooks.

2. **Message cloning.** `Message::clone()` is a virtual method that all SLICC-generated message classes override. If `m_traceId` is a field on the `Message` base class, `clone()` copies it automatically along with all other base fields. No per-protocol change is needed.

The gap is protocol messages that are *newly constructed* (not cloned) in response to an incoming traced message.

For example, a cache controller that receives a `GETX` request and constructs a new `DataResponse` message will not automatically carry the trace identity from the request.

That implies a staged approach.

Stage 1 should rely on root request tracing plus queue and flit events.

`m_traceId` propagates automatically through `RubyRequest` creation and `clone()`.

Newly constructed response messages will have `m_traceId == 0` and will not generate queue or flit events, but the root request's port-crossing and sequencer-callback events still provide a complete end-to-end lifetime.

Stage 2 can add explicit `m_traceId` propagation in SLICC action code for the most important protocol message constructions.

Start with one commonly used protocol such as MESI Two Level.

The eventual clean solution is to modify the SLICC compiler to automatically insert `m_traceId` propagation in generated message construction code when the source message is available in the action context.

That is valuable, but it should not block the first usable version.

## Future O3 Extension Path

This architecture is intentionally not Ruby-specific.

Later, an O3 instruction transaction can be created at fetch or decode using the same recorder.
The instruction transaction lives on a separate stream at the CPU pipeline front-end (e.g. `system.cpu0.fetch`), because instructions originate in the pipeline, not at the sequencer.

That instruction transaction would then parent child nodes such as:

1. decode token
2. LSQ memory request
3. cache miss request
4. Ruby message tree

A memory-request transaction on `system.ruby.l1_cntrl0.sequencer` becomes a child of the instruction transaction on `system.cpu0.fetch` via an explicit FTR relation.
The two transactions live on different streams (different originators), but the relation arrow connects them in the viewer.

The same event vocabulary remains useful.

`Created`, `Accepted`, `Enqueue`, `Dequeue`, `Issued`, `Callback`, and `Retire` all map naturally onto pipeline stages.

This is why the core framework should live under a generic simulation tracing location rather than under a Ruby-only directory.

## Implementation Plan

The implementation is organized around instrumenting framework primitives first, then adding domain-specific detail.

After steps 1 through 4, every Ruby request has a useful end-to-end lifetime trace before any Garnet or protocol-specific work begins.

### Step 1. Define Generic Trace Core

Create a generic tracing area, for example under `src/sim/transaction_trace/`.

Add:

1. `FtrTrace.py`
2. `ftr_trace.hh`
3. `ftr_trace.cc`
4. a small common header for trace IDs and attribute types
5. build integration and one debug flag

Implement:

1. file lifecycle
2. monotonic ID allocator
3. live transaction table
4. node creation API
5. event stamping API
6. retirement API
7. output flush on exit and drain-safe flush helpers

### Step 2. Add Trace Identity Fields

This step adds trace identity at three levels of the object hierarchy.

#### 2a. `TraceContext` Extension On `Request`

Define a `TraceContext` extension type for `Extensible<Request>` carrying root trace metadata.

Fields: `trace_id`, `root_trace_id`, `parent_trace_id`, `origin_tick`.

Do not duplicate fields already on `Request` (requestor ID, PC, etc.).

Add helpers to:

1. check whether a request already has trace context
2. create and attach one
3. retrieve root trace ID cheaply from any later path holding a `RequestPtr`

#### 2b. `trace_id` Field On `Message`

Add a `TraceId m_traceId = 0` field to the `Message` base class in `src/mem/ruby/slicc_interface/Message.hh`.

This bridges the gap between `Request`-anchored trace context and Ruby's internal message system.
In the first version it stores the root request trace ID only.

Set it from the `TraceContext` on the originating `Request` when `RubyRequest` is created.

For protocol messages derived from a traced request, copy `m_traceId` from the originating message.

#### 2c. `trace_id` Field On `flit`

Add a `TraceId m_traceId = 0` field to the `flit` class in `src/mem/ruby/network/garnet/flit.hh`.

Set it during `flitisizeMessage()` by allocating a new flit child node under the root request ID stored in `msg_ptr->m_traceId`.

This gives router stage ProbePoints a cheap way to report flit identity without chasing `m_msg_ptr`.

This step is the key future-proofing move.

### Step 3. Instrument Port-Level Timing Protocol (Automatic)

This is the highest-leverage step.

Add FTR notification calls to `TimingRequestProtocol::sendReq()` and `TimingResponseProtocol::sendResp()` in `src/mem/protocol/timing.cc`.

The logic for requests is:

1. Check `FtrTrace::get()`. If null, skip (zero-cost when tracing is off).
2. Check whether `pkt->req` already carries a `TraceContext` extension.
3. If it does, stamp a `PortCrossing` event with the peer port's owner `SimObject::name()` and `curTick()`.
4. If it does not, check whether the recorder's filter policy matches this request (see below). If it matches, create a pending root `MemoryRequest` transaction and attach a `TraceContext` extension to `pkt->req`.

The logic for responses is:

1. Check `FtrTrace::get()` and `pkt->req->getExtension<TraceContext>()`. If either is null, skip.
2. Stamp a `PortCrossing` event.
3. The `Retire` event is not stamped here — it is stamped by the Sequencer callback hook (Step 4), which knows the semantic completion reason (hit type, data source). The final port crossing is simply the last `PortCrossing` event in the trace.

#### Root Creation Filter Policy

`TimingRequestProtocol::sendReq()` does not and should not know whether it is entering Ruby, Classic caches, or any other subsystem.

Instead, the `FtrTrace` recorder owns a filter policy that decides which requests get root transactions.

For the first implementation, the filter can be a simple callback registered by `RubyPort` during `regProbeListeners()`:

```cpp
ftrTrace->setRootFilter([](PacketPtr pkt, ResponsePort *peer) {
    return dynamic_cast<RubyPort::MemResponsePort*>(peer) != nullptr
           && !pkt->req->hasExtension<TraceContext>();
});
```

This keeps the port protocol generic while allowing Ruby to opt in to root creation.

Later, additional filter policies can be registered for Classic caches, DMA, or O3 LSQ entry points.

This generalizes the existing `addTrace()` / `removeTrace()` pattern that already executes on every timing transaction.

After this step, every port crossing in the entire system is automatically traced.

The trace shows request entry into Ruby, internal port crossings, and response path without any per-component instrumentation.

### Step 4. Add Domain-Specific Attribute Capture In Sequencer (Manual)

Instrument the Sequencer to record domain-specific attributes that the port-level primitive cannot infer.

1. `Sequencer::makeRequest()` — finalize the pending root transaction as live. Record static attributes (address, request type, size, flags) by reading them from `pkt->req` directly. Set `m_traceId` on the `RubyRequest` being created so that trace identity flows into Ruby's `Message` layer.
2. `Sequencer::issueRequest()` — stamp `RubyIssued` event.
3. `Sequencer::readCallback()` / `writeCallback()` / `atomicCallback()` — stamp `SequencerCallback` event with hit/miss type and data source. Stamp `Retire` event to close the root transaction.

The `m_traceId` propagation in point 1 is critical: it bridges trace identity from `Request` (port world) into `Message` (Ruby world), enabling the automatic `MessageBuffer` hooks in Step 5.

These are approximately three manual hook points.

At the end of this step, every Ruby request has a useful lifetime trace including request attributes, port crossings, issue timing, and completion timing, even before network details are added.

### Step 5. Instrument MessageBuffer (Automatic)

Make `MessageBuffer` trace-aware in its base implementation.

Add an optional `FtrTrace*` recorder pointer set during configuration.

When non-null:

1. `enqueue()` emits an `Enqueue` event with the buffer's `SimObject::name()`, `curTick()`, queue occupancy, and vnet.
2. `dequeue()` emits a `Dequeue` event with the same fields.

The event is stamped on the root request node identified by `message->m_traceId`.

In the first version, `Message::m_traceId` is root identity only.
`MessageBuffer` does not attempt to infer or create a more specific message-local node.

This covers all queue timestamps for all protocols and all controllers with one code change.

### Step 6. Instrument NetworkInterface For Flit Children (Automatic)

Instrument `NetworkInterface::flitisizeMessage()` once.

Create:

1. one `Flit` child transaction per flit

Record parent-child relations explicitly.

Stamp injection and per-flit creation immediately.

Record static attributes: parent root trace ID, vnet, VC, message size, flit count, source NI name, destination node, flit index, and Garnet packet ID.

For clarity in v1:

1. the new flit child gets a fresh `trace_id`
2. `root_trace_id` is copied from `msg_ptr->m_traceId`
3. `parent_trace_id` is set to that same root request ID

Instrument destination-side `NetworkInterface::wakeup()` for ejection and reassembly events.

This is a single-funnel change because all Garnet traffic enters and exits through `NetworkInterface`.

### Step 7. Add Router Stage ProbePoints (Automatic)

Add one `ProbePointArg<FlitTraceStamp>` to each Garnet router pipeline stage.

1. `InputUnit::wakeup()` — `probeRouterArrive`
2. `SwitchAllocator::wakeup()` — `probeSwitchAlloc`
3. `CrossbarSwitch::wakeup()` — `probeSwitchTraverse`
4. `NetworkLink::wakeup()` — `probeLinkTraverse`

The `FtrTrace` SimObject registers as a `ProbeListener` for these probe points during `regProbeListeners()`.

Keep the event payloads small and stage oriented: flit trace ID, router or link name, tick, VC, outport.

Do not dump large repeated route structures on every event.

If `FtrTrace` is not instantiated, the probes fire to zero listeners at negligible cost.

### Step 8. Add Protocol-Aware Message Nodes Where Worthwhile (Manual)

After the primitive-level instrumentation is proven useful, add richer `RubyMessage` child-node creation at key protocol emission sites.

Start with one commonly used protocol such as MESI Two Level.

Use that work to decide whether message-level trace metadata belongs in SLICC-generated message classes or in a narrow side-table bridge.

This is the only step that requires per-protocol work.

### Step 9. Add Validation And Documentation

Add tests that verify:

1. monotonic and unique ID allocation
2. correct parent-child relationships
3. retirement of root and child nodes
4. stable dotted object names
5. correct event ordering for a simple Ruby request
6. flit split counts versus packet size and link width
7. automatic port-crossing events appear without per-component instrumentation
8. queue events appear for all protocols without protocol-specific hooks
9. router stage events appear via ProbePoint listeners

Add one smoke configuration that produces a small FTR file for a Ruby random test or synthetic Garnet traffic run.

## Verification Strategy

Use three levels of verification.

### Unit Tests

Test recorder semantics without running a full simulation.

Focus on IDs, node ownership, retirement, and serialization.

### Targeted Integration Tests

Run a small Ruby configuration and verify:

1. one root transaction per accepted request
2. final retirement exists
3. port-crossing events appear automatically without per-component hooks
4. queue events appear for MessageBuffers that handle traced messages
5. queue events do not appear for messages with `m_traceId == 0`
6. flit child count matches expected packet decomposition
7. `m_traceId` survives message cloning through `clone()`
8. flit `m_traceId` matches parent message `m_traceId`

### Human Inspection

Open the trace in the intended FTR tooling and confirm that dotted hierarchy names match the SimObject design hierarchy and that request trees are easy to follow.

## Risks And Mitigations

### Risk: Too Much Protocol Coupling

Mitigation: primitive-level instrumentation eliminates protocol coupling entirely for port crossings, queue events, and network events.

Only the three manual Sequencer hooks and optional protocol-aware message nodes (Step 8) touch protocol-specific code.

### Risk: High Runtime Overhead

Mitigation: the primitive-level approach naturally supports zero-cost-when-disabled.

Port-level hooks check for `TraceContext` extension presence, which is a null pointer test when tracing is off.

`MessageBuffer` hooks check a recorder pointer, which is null when tracing is off.

Router stage ProbePoints fire to zero listeners when `FtrTrace` is not instantiated.

For fine-grain control, support event filtering by node kind, object path prefix, vnet, or event kind.

### Risk: Pointer-Lifetime Correlation Bugs

Mitigation: put durable semantics on `Request` extension data and recorder IDs, not on transient pointers alone.

The primitive-level approach reinforces this because the `TraceContext` on `Request` is the single source of truth consulted at every port crossing and queue operation.

### Risk: Trace Explosion On Large Networks

Mitigation: support event filtering by node kind, object path prefix, vnet, or event kind.

Router stage ProbePoints can be selectively registered per router group.

### Risk: Hard To Extend To O3 Later

Mitigation: the port-level instrumentation already covers O3 LSQ memory requests automatically because they use the same `TimingRequestProtocol::sendReq()` path.

O3 pipeline-internal tracing would add new ProbePoints at pipeline stages, following the same pattern as Garnet router stages.

### Risk: Primitive-Level Hooks May Be Too Coarse

Mitigation: the three manual Sequencer hooks provide domain-specific detail that the primitives cannot infer.

The design explicitly supports adding more manual hooks later where the primitive-level granularity is insufficient.

The primitive-level hooks provide the structural backbone, and manual hooks add semantic richness on top.

## Recommended Final Shape

The best transaction trace for gem5 is a generic, hierarchical, event-driven transaction recorder whose instrumentation is embedded in framework primitives rather than scattered across individual components.

The durable root identity should live on `Request` through a `TraceContext` extension object.

The runtime recorder should be a `SimObject`.

Object names written to FTR should always reuse canonical SimObject dotted hierarchy names.

Port-level timing protocol methods should automatically stamp every port crossing system-wide.

`MessageBuffer` should automatically stamp every queue operation for all protocols.

`NetworkInterface::flitisizeMessage()` should automatically create flit child transactions.

Garnet router stages should emit ProbePoints that the recorder listens to.

Only domain-specific attribute capture in the Sequencer and optional protocol-aware message nodes require manual instrumentation.

That combination gives a useful first implementation now, automatic coverage for future components, and a clean path to future O3 instruction tracing later.

## Future Consideration: Perfetto As An Alternative Or Complementary Trace Backend

The internal trace model described in this plan (hierarchical spans with begin/end ticks, parent-child relationships, and key-value attributes) is structurally very close to what Google's Perfetto tracing system represents natively.

Perfetto's data model supports nested spans (slices), flow events (causal links between spans), track-scoped events, and arbitrary debug annotations, which map naturally onto the transaction nodes and events described here.

Before finalizing the internal schema and serialization, it is worth evaluating whether adopting Perfetto's protobuf-based trace format as an output backend (alongside or instead of raw FTR) would be beneficial.

Potential advantages:

1. Perfetto UI provides a mature, interactive trace viewer out of the box with hierarchical span visualization, search, and filtering.
2. The trace format is well-documented and battle-tested at scale.
3. Causal links between spans (Perfetto flow events) would allow expressing "this miss caused that network message" relationships that pure parent-child trees cannot capture.
4. During development of the tracing framework itself, having a second viewer for free would accelerate debugging of the trace output.

This evaluation should happen after the core recorder API and the first vertical slice of instrumentation are working.
The internal recorder design should not hard-wire itself to one output format.
A narrow writer interface (as already recommended) would allow adding a Perfetto backend later without changing instrumentation code.

## If You Remember One Thing

Do not instrument components.

Instrument primitives.

gem5 has four framework funnels (port protocol, `MessageBuffer`, `NetworkInterface`, Garnet ProbePoints) through which all traffic flows.

Embed tracing there once, and every current and future component gets traced automatically.

Manual hooks should only capture domain-specific attributes that the primitives cannot infer.
