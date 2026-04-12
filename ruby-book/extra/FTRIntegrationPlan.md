# Plan: Integrate FTR Transaction Tracing into gem5 and Ruby

## Table of Contents

- [Problem Statement](#problem-statement)
- [Goals](#goals)
- [Non-Goals (First Version)](#non-goals-first-version)
- [Data Model](#data-model)
  - [Streams and Generators](#streams-and-generators)
  - [Transaction Nodes](#transaction-nodes)
  - [Events on Transactions](#events-on-transactions)
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
  - [Output Format](#output-format)
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
| `system.ruby.l1_cntrl0.sequencer` | `Sequencer` | `memreq`, `memreq.events`, `flit`, `flit.events` |
| `system.ruby.l1_cntrl1.sequencer` | `Sequencer` | `memreq`, `memreq.events`, `flit`, `flit.events` |
| `system.ruby.l1_cntrl2.sequencer` | `Sequencer` | `memreq`, `memreq.events`, `flit`, `flit.events` |
| `system.ruby.l1_cntrl3.sequencer` | `Sequencer` | `memreq`, `memreq.events`, `flit`, `flit.events` |

The `memreq` generator produces root `MemoryRequest` transactions.
The `flit` generator produces `Flit` child transactions.
The `.events` companion generators produce zero-duration event transactions (see [Events on Transactions](#events-on-transactions)).
All generators live on the same stream so that a request, its flit children, and all their events appear together on the same viewer swim-lane.

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

### Events on Transactions

FTR has no native "event" primitive.
Following the pattern established by LWTR4SC (`feature/record_events` branch), events are implemented as **zero-duration child transactions** on a companion `.events` generator, linked to the parent transaction via a `"parent_of"` relation.

This means every event is a full FTR transaction with `begin_time == end_time`, carrying its own attributes.
No changes to the FTR writer or file format are required — events are serialized identically to regular transactions.

Each event transaction carries these attributes:

| Attribute | Type | Description |
|-----------|------|-------------|
| `event_kind` | `STRING` | What happened: `port_crossing`, `enqueue`, `dequeue`, `router_arrive`, `switch_alloc`, `switch_traverse`, `link_traverse`, `issue`, `completion` |
| `object_name` | `STRING` | `SimObject::name()` where it happened |
| `stage_name` | `STRING` | Optional sub-object stage (e.g. `switch_allocator`) |
| *(additional)* | varies | Optional key-value pairs (queue occupancy, VC, outport, etc.) |

`event_kind` must always be recorded as an explicit attribute.
LWTR4SC passes event name as a function parameter but never writes it to the trace — we must not repeat that bug.

Flit pipeline stages (router arrival, switch allocation, crossbar traverse, link traverse) are events on the flit's own transaction — not separate transaction nodes.

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
If Ruby rejects the request for retry, the `sendResp` hook detects the root is still in pending state (never finalized by the Sequencer) and removes the `TraceContext` extension from `pkt->req`, discarding the pending root from the live transaction table.
No committed transaction is written to the trace file.
This keeps cleanup symmetric with creation — both happen in the port layer, requiring no changes to the Sequencer.

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

### Output Format

`FtrTrace` supports two output backends, selected by a Python parameter:

```python
class FtrTrace(SimObject):
    type = 'FtrTrace'
    cxx_header = 'sim/transaction_trace/ftr_trace.hh'

    output_format = Param.String('ftr', "Output format: 'ftr' (binary) or 'text'")
    output_file = Param.String('transactions', "Base filename (extension added automatically)")
```

| Format | Extension | Description |
|--------|-----------|-------------|
| `ftr` | `.ftr` | Binary CBOR with LZ4 compression via `ftr::ftr_writer<true>`. Compact, suitable for production runs. Viewable in SCViewer. |
| `text` | `.txlog` | Human-readable text dump via LWTR4SC's text backend. One line per transaction/event/relation. Useful for initial bringup, debugging, and `grep`/`diff`-based validation. |

Both backends implement the same internal writer interface, so the recorder API and all instrumentation code are format-agnostic.
The writer is instantiated once during `FtrTrace` construction based on the `output_format` parameter.

The text format is especially valuable during development:
- Readable without a viewer — `grep`, `tail -f`, `diff` against expected output.
- Eliminates the question "is the binary output wrong or is my viewer misinterpreting it?"
- Makes integration test assertions trivial: match lines in the text output.

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

#### What `stampEvent` maps to in FTR

`stampEvent()` is a convenience API.
Internally it maps to FTR primitives following the LWTR4SC pattern:

1. Create a zero-duration transaction on the parent's `.events` companion generator at `tick` (`begin_time == end_time == tick`).
2. Record `event_kind` as a `STRING` attribute on the event transaction.
3. Record `object_name` as a `STRING` attribute.
4. Record all entries from `attrs` as additional attributes.
5. Add a `"parent_of"` relation with the parent as `src` and the event as `sink` (matching LWTR4SC's convention where `add_relation` caller = sink, argument = source).

The caller does not need to know about companion generators or relations — `stampEvent` handles the mapping.
Always pass an explicit `tick` (gem5's `curTick()`), matching LWTR4SC's `record_event_at_time` variant rather than the implicit-time `record_event`.

---

## Implementation Steps

### Step 1. Generic Trace Core — DONE

Created `src/sim/transaction_trace/` with:
- `FtrTrace.py` — SimObject definition with `output_format` and `output_file` parameters
- `ftr_trace.hh` / `ftr_trace.cc` — `FtrTrace` SimObject wrapping `tx_trace::TxTrace`; singleton access via `get()`; lazy per-sequencer stream/generator creation; pending-root lifecycle (reserve ID → finalize or discard); root filter policy; drain-safe flush
- `trace_context.hh` — `TraceContext` extension on `Request` (traceId, rootTraceId, parentTraceId, originTick)
- `SConscript` updated with `SimObject()`, `Source()`, `DebugFlag('TxTrace')`

Extended `ext/tx_trace/` library with:
- `reserveId()` — allocate ID without writing to the trace (for deferred pending roots)
- `createRootTransactionWithId()` / `createChildTransactionWithId()` — use pre-reserved IDs

Unit tests: 12 tests pass (4 new tests for reserved-ID API).

### Step 2. Trace Identity Fields — DONE

**2a.** `TraceContext` extension on `Request` via `Extension<Request, TraceContext>` in `src/sim/transaction_trace/trace_context.hh`.

**2b.** `uint64_t m_rootTraceId = 0` added to `Message` base class in `src/mem/ruby/slicc_interface/Message.hh` with `getRootTraceId()` / `setRootTraceId()` accessors. Default copy propagates automatically via `Message(const Message&) = default`, so `clone()` works.

**2c.** `uint64_t m_traceId = 0` added to `flit` class in `src/mem/ruby/network/garnet/flit.hh` with `getTraceId()` / `setTraceId()` accessors.

### Step 3. Port-Level Instrumentation — DONE

FTR calls added to `TimingRequestProtocol::sendReq()` and `TimingResponseProtocol::sendResp()` in `src/mem/protocol/timing.cc`.

Request path (`sendReq`):
1. `FtrTrace::get()` null → skip.
2. `pkt->req` has `TraceContext` → stamp `port_crossing` event.
3. No `TraceContext` + filter matches → create pending root, attach `TraceContext`.
4. If `recvTimingReq` returns false (rejected) and we created a pending root → `discardPendingRoot()`, remove `TraceContext`.

Response path (`sendResp`):
1. `FtrTrace::get()` null or no `TraceContext` → skip.
2. Stamp `port_crossing` event.

### Step 4. Sequencer Hooks — DONE

Three manual hooks in `src/mem/ruby/system/Sequencer.cc`:
1. `makeRequest()`: after `insertRequest()` succeeds, finalize pending root as live, record static attributes (address, line_address, size, type, requestor_id, PC).
2. `issueRequest()`: after `RubyRequest` msg is constructed, bridge trace ID into Message layer (`msg->setRootTraceId()`), stamp `issue` event.
3. `hitCallback()`: stamp `completion` event with `external_hit` and `machine` attributes, retire root transaction. Placed before `ruby_hit_callback(pkt)`.

Filter registration in `RubyPort::init()` (`src/mem/ruby/system/RubyPort.cc`): only requests entering Ruby through `MemResponsePort` that don't already have a `TraceContext` get root transactions.

After this step, every accepted Ruby request produces a complete root transaction with static attributes, issue event, completion event, and retirement.

### Step 5. MessageBuffer Instrumentation — DONE

Trace-awareness added to `MessageBuffer::enqueue()` and `dequeue()` in `src/mem/ruby/network/MessageBuffer.cc`:

```cpp
// In enqueue(), after push_heap:
if (auto *ftr = FtrTrace::get();
    ftr && message->getRootTraceId() != 0) {
    ftr->stampEvent(message->getRootTraceId(), "enqueue",
                    name(), current_time,
                    {{"occupancy", uint64_t(m_prio_heap.size())},
                     {"vnet", uint64_t(message->getVnet())}});
}
```

Covers queue crossings for messages that carry a nonzero `m_rootTraceId` — in v1 this means `RubyRequest` and its clones.
Protocol-generated messages (responses, forwards, writebacks) that are constructed fresh rather than cloned will have `m_rootTraceId == 0` and are silently skipped.
Full protocol-message coverage requires the per-protocol propagation work in Step 8.

### Step 6. NetworkInterface Flit Children — DONE

In `flitisizeMessage()` (`src/mem/ruby/network/garnet/NetworkInterface.cc`): after each `flit` object is created, if `new_msg_ptr->getRootTraceId() != 0`, create a child transaction via `FtrTrace::createFlitChild()` with attributes (flit_index, packet_id, vnet, vc, num_flits, dest_ni) and set `fl->setTraceId()`.

In destination-side `wakeup()`: retire flit transaction at all three ejection paths (normal tail-flit ejection, non-tail flit consumption, stall-queue unstall).

### Step 7. Router Stage Events — DONE

Direct `FtrTrace::get()` calls (not ProbePoints) in the four Garnet pipeline stages — simpler, same zero-cost-when-disabled guarantee:

| Stage | File | Event | Extra attributes |
|-------|------|-------|-----------------|
| `InputUnit::wakeup()` | `InputUnit.cc` | `router_arrive` | vc |
| `SwitchAllocator::arbitrate_outports()` | `SwitchAllocator.cc` | `switch_alloc` | invc, outvc, outport |
| `CrossbarSwitch::wakeup()` | `CrossbarSwitch.cc` | `switch_traverse` | — |
| `NetworkLink::wakeup()` | `NetworkLink.cc` | `link_traverse` | — |

### Step 8. Protocol-Aware Trace ID Propagation via SLICC Compiler — NOT STARTED

**Status: Critical next step. Unblocks flit, router-stage, and per-protocol queue events.**

#### Problem

Steps 5–7 are fully implemented but still produce no flit/router output for most protocol traffic because fresh SLICC messages default to `m_rootTraceId == 0`.

The naive fix is to teach each generated `enqueue(...)` block to copy from a lexically visible `in_msg_ptr`.
That is **not** robust enough for real protocols.

SLICC actions are emitted as standalone controller methods, so many `enqueue(...)` sites do not have a usable `in_msg_ptr` in scope even though they are still causally downstream of a traced input message.
CHI is the clearest counterexample:

```
RubyRequest (trace=N)
    ▼
peek(seqInPort, RubyRequest)
    ▼
enqueue(reqRdyOutPort, CHIRequestMsg)          // first propagation hop
    ▼
peek(reqRdyPort, CHIRequestMsg)
    ▼
Initiate_Request allocates TBE and schedules TriggerMsg
    ▼
peek(triggerInPort, TriggerMsg)
    ▼
Send_ReadShared / Send_ReadNoSnp / Send_Snp*   // no original RubyRequest in scope
    ▼
enqueue(reqOutPort / snpOutPort / rspOutPort / datOutPort)
```

Any Step 8 design that only works when the final emission site can directly name `in_msg_ptr` will fail for CHI and for similar multi-stage SLICC controllers that bounce through internal ready/trigger queues.

#### Design decision: propagate the ID, not a pointer

- `Message::m_rootTraceId` (uint64_t) is the durable identity. The trace recorder holds the metadata keyed by this ID.
- A pointer to the original `Message` adds nothing for tracing and introduces lifetime/serialization problems (MsgPtr is shared_ptr — extends root lifetime arbitrarily; breaks checkpointing).
- The ID is globally unique, monotonic, and functionally equivalent to a pointer for identity purposes.

#### Design decision: compiler-managed controller trace context

- **Per-protocol `.sm` annotation** (add `out_msg.m_rootTraceId := in_msg.m_rootTraceId;` at every emission site): works but requires modifying every protocol, risks being forgotten, and fails for protocols added later.
- **Controller-level hook** (copy by address lookup): fragile, can't distinguish coalesced requests, doesn't handle writebacks/forwards cleanly.
- **Lexical `in_msg_ptr` injection in `EnqueueStatementAST` only**: insufficient because actions are generated as standalone methods and later-stage emissions often do not have the original input message in lexical scope.
- **SLICC compiler + controller trace context**: `peek(...)` establishes the current root trace ID on the controller, and every later `enqueue(...)` / `deferEnqueueing(...)` inherits from that context.
  This covers all current and future SLICC protocols automatically, including CHI's ready/trigger queue pipeline.

#### Implementation

**1. Add controller trace-context support in `AbstractController`.**

Files:
- `src/mem/ruby/slicc_interface/AbstractController.hh`
- `src/mem/ruby/slicc_interface/AbstractController.cc`

Add:
- `uint64_t m_currentRootTraceId = 0;`
- `uint64_t getCurrentRootTraceId() const;`
- a small RAII helper such as `ScopedRootTraceContext` that saves the previous value, installs a new one on construction, and restores the previous value on destruction

The helper must support:
- nested `peek(...)` scopes
- exception-safe restoration when `RejectException` is thrown
- zero-cost behavior when the current ID is zero

**2. Teach `PeekStatementAST` to establish controller context from the message being processed.**

File:
- `src/mem/slicc/ast/PeekStatementAST.py`

After the successful `dynamic_cast`, emit generated C++ equivalent to:

```cpp
auto trace_guard = scopedRootTraceContext(in_msg_ptr->getRootTraceId());
```

This makes the currently processed message's root ID available to the whole generated body, including:
- direct `enqueue(...)` calls inside the same `peek(...)`
- helper-function calls that later enqueue messages
- standalone action methods invoked by the current transition

**3. Stamp fresh messages from controller context in `EnqueueStatementAST`.**

File:
- `src/mem/slicc/ast/EnqueueStatementAST.py`

After the generated body statements and immediately before the generated queue operation, emit:

```cpp
out_msg->setRootTraceId(getCurrentRootTraceId());
```

Stamping at the end of the block is deliberate.
It ensures helper code and field assignments run first, and the trace field is finalized just before the message becomes visible to the queue/network.

**4. Apply the same rule to deferred messages.**

File:
- `src/mem/slicc/ast/DeferEnqueueingStatementAST.py`

Before `deferEnqueueingMessage(addr, out_msg)`, emit:

```cpp
out_msg->setRootTraceId(getCurrentRootTraceId());
```

This keeps deferred-message paths consistent with normal enqueue paths.

**5. Define the propagation rule in terms of active controller context, not lexical scope.**

| Situation | Behavior |
|---|---|
| `enqueue(...)` or `deferEnqueueing(...)` executed while processing a message whose `peek(...)` installed root ID `N` | Stamp `out_msg->m_rootTraceId = N` |
| Nested `peek(...)` on another message while already inside a traced flow | Temporarily override with the nested message's root ID, then restore on scope exit |
| Internal queues (`reqRdy`, `snpRdy`, `triggerQueue`, `retryTriggerQueue`, etc.) carrying traced messages | Their messages inherit the active root ID and later re-establish it when peeked |
| Timer-driven / wakeup-driven / maintenance work with no active traced input | `getCurrentRootTraceId() == 0`, so the new message remains untraced |

**Fanout semantics (for v1):** when one input triggers N output messages (e.g., directory invalidating N sharers), all N carry the same trace ID. Sub-transactions with explicit `parent_of` relations per fanout branch are a future extension.

#### Why this works for CHI

CHI is explicitly **in scope** for this step because its request, response, data, trigger, retry-trigger, and replacement messages are all SLICC `interface="Message"` types.

The revised rule works cleanly across CHI's multi-stage pipeline:

1. `peek(seqInPort, RubyRequest)` accepts the traced sequencer request and installs its root ID on the controller.
   `enqueue(reqRdyOutPort, CHIRequestMsg, ...)` therefore stamps the first internal `CHIRequestMsg` with the same root ID.
2. Later, `peek(reqRdyPort, CHIRequestMsg)` re-establishes that same root ID.
   Any `TriggerMsg`, `RetryTriggerMsg`, or other internal messages created while handling the request inherit it automatically.
3. When CHI later processes `peek(triggerInPort, TriggerMsg)` and executes actions such as `Send_ReadShared`, `Send_ReadNoSnp`, `Send_Snp*`, `Send_*Rsp`, or `Send_*Data`, the controller context is again active, so the final `reqOutPort`, `snpOutPort`, `rspOutPort`, and `datOutPort` messages inherit the same root ID.

This is exactly why a lexical `in_msg_ptr` solution is insufficient and why the controller-context design is the right implementation boundary.

CHI-specific expected outcomes:
- traced sequencer requests propagate through `reqRdy` and later network outports without any per-protocol `.sm` edits
- trigger-driven downstream emissions retain the original root trace ID
- internally generated maintenance work that starts with no traced input still stays untraced

**Representative scenarios that must work after the change:**

| Scenario | Triggering `in_msg` | Result |
|---|---|---|
| L1 miss issues GETS to directory | RubyRequest with trace ID | Propagated ✓ |
| Directory forwards to owner | Forward request carrying ID | Propagated ✓ |
| Directory fans out invalidations to sharers | Original request | All invalidations share ID ✓ |
| CHI `RubyRequest -> reqRdy -> triggerQueue -> reqOutPort` | RubyRequest, then CHIRequestMsg, then TriggerMsg | Same root ID survives every hop ✓ |
| CHI snoop/data/response paths | CHIRequestMsg / TriggerMsg / CHIDataMsg / CHIResponseMsg | Same root ID preserved across ready and trigger queues ✓ |
| Writeback from L1 eviction | No triggering in_msg | ID stays 0 — correctly skipped ✓ |
| Hardware prefetch | No CPU-originated trace | ID stays 0 — correctly skipped ✓ |

**Non-goals for this step:**
- Adding new message fields (already covered — `m_rootTraceId` lives on `Message` base class).
- Non-SLICC controller stacks outside Ruby's SLICC-generated protocol machines.
- Changing the transaction model from "one root ID per causal protocol flow" to explicit per-message child transactions.
- Sub-transaction nodes for protocol message stages — future extension (Step 10).

#### Testing methodology

**Test 1: SLICC codegen pyunit**

Use the existing Python unit-test harness instead of a new GTest.
Parse fixture `.sm` files into a temporary output directory and inspect the generated controller C++.

Assert:
- `PeekStatementAST` emits the scoped controller-trace guard immediately after a successful cast
- `EnqueueStatementAST` emits `out_msg->setRootTraceId(getCurrentRootTraceId())` immediately before `buffer.enqueue(...)`
- `DeferEnqueueingStatementAST` emits the same stamping before `deferEnqueueingMessage(...)`
- nested `peek(...)` scopes generate nested guards
- fixture code that has no active `peek(...)` context still compiles and uses `getCurrentRootTraceId()` safely

Include one fixture specifically modeling the CHI-style two-hop case:
- first `peek(seqInPort, RubyRequest)` enqueues to an internal ready queue
- later `peek(reqRdyPort, SomeMsg)` triggers a standalone action that enqueues again without a direct `RubyRequest` in lexical scope

**Test 2: Protocol compile regression**

Build at least:
- `MI_example`
- `MESI_Two_Level`
- `MESI_Three_Level`
- `MOESI_CMP_directory`
- `MOESI_AMD_Base`
- `MOESI_hammer`
- `CHI`

No protocol behavior should change except for trace metadata becoming nonzero on propagated messages.

**Test 3: End-to-end smoke test on MI_example**

Re-run `ruby-book/final/smoke/ftr_smoke_test.py` after the fix. Expected deltas from the pre-fix trace:

| Metric | Before | After (expected) |
|---|---|---|
| Flit child transactions | 0 | > 0, roughly `num_flits_per_request × completed_requests` |
| `router_arrive` events | 0 | > 0, matches `m_router_stats.m_flits_received.total()` |
| `switch_alloc` events | 0 | > 0, matches `SwitchAllocator` activity count |
| `switch_traverse` events | 0 | > 0, matches crossbar activity count |
| `link_traverse` events | 0 | > 0, matches `m_link_utilized` across all links |
| Queue `enqueue`/`dequeue` events | only mandatoryQueue | all Ruby queues that carry traced messages |

The Garnet stats in `stats.txt` provide ground truth for expected event counts.

**Test 4: CHI-specific validation**

Run an existing CHI harness such as:
- `tests/gem5/chi_protocol/configs/chi-with-isa.py`

Assertions:
- traced requests produce nonzero `enqueue` / `dequeue` events on CHI internal queues such as `reqRdy`, `triggerQueue`, and `retryTriggerQueue` when those queues participate in the flow
- later `reqOut`, `snpOut`, `rspOut`, and `datOut` messages carry the same root trace ID as the originating traced request
- Garnet flit child transactions and router-stage events become nonzero for traced CHI traffic
- a sampled CHI flow can be reconstructed as:
  `RubyRequest -> reqRdy CHIRequestMsg -> TriggerMsg -> reqOut/snpOut/rspOut/datOut`
  with one stable `rootTraceId` across all hops

This is the explicit double-check that the revised Step 8 design works with CHI and not only with the simpler MESI/MI protocols.

**Test 5: Counting invariant**

For each completed root transaction, count child flits in the trace and compare to the expected number of flits per request (function of message size and link width, available from GarnetNetwork params). Assert:
```
total_flit_children == sum over roots of expected_flits_per_request
```
within a tolerance for requests still in flight at simulation end.

**Test 6: Fanout correctness**

Configure a multicast-heavy scenario (e.g., MOESI_CMP_directory with shared-line reads from multiple cores). Assert that invalidations to different sharers carry the same `rootTraceId` and appear as separate flit children of the same root.

#### Effort estimate

2–3 days including SLICC compiler changes, `AbstractController` runtime support, pyunit fixtures, CHI validation, and protocol regression runs.

### Step 9. Validation and Documentation — PARTIAL

**Done:**
- Unit tests: 12 tests covering ID allocation, parent-child relationships, retirement, serialization, reserved-ID API.
- Smoke test: `ruby-book/final/smoke/ftr_smoke_test.py` — 2-CPU Garnet Mesh_XY with MI_example, produces `transactions.txlog` with root transactions, issue/completion/enqueue/dequeue events.
- Build verification: `scons build/RISCV/gem5.opt` compiles cleanly; `scons build/NULL/unittests.opt` passes all tests.

**Remaining:**
- Integration tests for flit child count (blocked on Step 8).
- SCViewer inspection of binary FTR output (blocked on binary backend).
- Smoke test validation of flit and router-stage events (blocked on Step 8).

---

### Smoke Test Results (v1, Steps 1–7)

Run: `gem5.opt ftr_smoke_test.py --protocol=MI_example --num-cpus=2 --num-dirs=2 --network=garnet --topology=Mesh_XY --mesh-rows=2`

| Metric | Value |
|--------|-------|
| Output file | `transactions.txlog` (983 KB) |
| Streams | 2 (one per sequencer) |
| Generators | 8 (memreq, memreq.events, flit, flit.events × 2) |
| Transactions started | 3,462 |
| Transactions ended | 3,431 (31 in-flight at simulation end) |
| Parent-child relations | 2,713 |
| `issue` events | 677 |
| `completion` events | 718 (includes coalesced requests) |
| `enqueue` events | 676 (mandatory queue only — RubyRequest) |
| `dequeue` events | 642 |
| `port_crossing` events | 0 (see note below) |
| Flit child transactions | 0 (blocked on Step 8) |
| Router stage events | 0 (blocked on Step 8) |

**Note on `port_crossing`:** Zero port-crossing events due to a bug — see Known Bugs below.

---

### Known Bugs

#### BUG: `stampEvent` fails after `retireTransaction` — breaks response-path `port_crossing` events

The plan's transaction lifecycle defines four phases:

```
Phase 1 (PENDING)  → sendReq creates pending root
Phase 2 (LIVE)     → Sequencer::makeRequest finalizes root
Phase 3 (RETIRED)  → Sequencer::hitCallback retires root
Phase 4 (FINAL)    → sendResp stamps last port_crossing event
```

Phase 4 should stamp a `port_crossing` event **after** the root is retired.
This fails because `FtrTrace::retireTransaction()` erases the TraceId from `liveTxs_`, and `stampEvent()` uses `liveTxs_` to look up the `.events` companion generator ID. Once the entry is gone, `stampEvent` silently skips.

The `TraceContext` extension is still on `pkt->req` (never removed), so the TraceId is available. The problem is purely that the generator mapping is lost.

**Timeline:**
1. `hitCallback()` → `ftr->retireTransaction(id)` → writes `tx_end`, **erases `liveTxs_[id]`**
2. `ruby_hit_callback(pkt)` → `schedTimingResp(pkt)`
3. `sendResp()` → `ftr->stampEvent(ctx->traceId, "port_crossing", ...)` → `liveTxs_.find(id)` returns `end()` → **skipped**

**Fix:** Keep the generator mapping available after retirement.
`stampEvent` creates its own independent zero-duration child transaction on the `.events` generator — it does not need the parent transaction to be open in the writer. It only needs the generator ID.
Options: (a) don't erase from `liveTxs_` in `retireTransaction`, add a `retired` flag instead and erase lazily; (b) move retired entries to a separate `retiredTxs_` map that preserves only the generator mapping; (c) store the generator ID in `TraceContext` on `pkt->req` so `stampEvent` doesn't need the lookup at all.

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
trace.writeGenerator(0, "memreq",        0);  // root memory requests
trace.writeGenerator(1, "memreq.events", 0);  // events on memory requests
trace.writeGenerator(2, "flit",          0);  // flit children
trace.writeGenerator(3, "flit.events",   0);  // events on flits

// Root memory request transaction
uint64_t tx = 1;
trace.startTransaction(tx, /*gen=*/0, /*stream=*/0, /*time=*/1000);
trace.writeAttribute(tx, ftr::event_type::BEGIN,
    "address", ftr::data_type::UNSIGNED, uint64_t(0x80001000));
trace.endTransaction(tx, /*time=*/1045);

// Event on root request (zero-duration child on memreq.events)
uint64_t evt = 2;
trace.startTransaction(evt, /*gen=*/1, /*stream=*/0, /*time=*/1005);
trace.writeAttribute(evt, ftr::event_type::RECORD,
    "event_kind", ftr::data_type::STRING, "enqueue");
trace.writeAttribute(evt, ftr::event_type::RECORD,
    "object_name", ftr::data_type::STRING,
    "system.ruby.l1_cntrl0.mandatoryQueue");
trace.endTransaction(evt, /*time=*/1005);  // begin == end
trace.writeRelation("parent_of", /*sink_stream=*/0, /*sink_tx=*/1,
                                  /*src_stream=*/0,  /*src_tx=*/2);

// Flit child transaction
uint64_t flit_tx = 3;
trace.startTransaction(flit_tx, /*gen=*/2, /*stream=*/0, /*time=*/1010);
trace.endTransaction(flit_tx, /*time=*/1040);
trace.writeRelation("parent_of", /*sink_stream=*/0, /*sink_tx=*/1,
                                  /*src_stream=*/0,  /*src_tx=*/3);
```

### Transaction Traces in Other Systems

| System | Span concept | Timeline concept | Causality concept |
|--------|-------------|-----------------|-------------------|
| FSDB (Synopsys Verdi) | Transaction | Stream | Parent handle |
| SystemC SCV | Transaction | Stream/Generator | Parent handle |
| OpenTelemetry | Span | Service | Parent span ID, span links |
| Perfetto | Slice | Track | Flow ID |

FTR inherits the SCV vocabulary directly and adds compact binary serialization with LZ4 compression.

### LWTR4SC Events API Reference

The `feature/record_events` branch of [LWTR4SC](https://github.com/Minres/LWTR4SC/tree/feature/record_events) demonstrates how events are implemented on top of the existing FTR primitives.
Events are zero-duration child transactions on a companion `<generator>.events` generator, linked via `"parent_of"` relations.
No FTR format changes are required.
Our `stampEvent()` API follows this pattern directly.
