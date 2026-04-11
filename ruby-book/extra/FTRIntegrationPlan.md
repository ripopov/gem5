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

### Step 8. Protocol-Aware Trace ID Propagation — NOT STARTED

**Status: This is the critical next step for enabling flit and router-stage tracing.**

Steps 6–7 are fully implemented but produce no output because they gate on `m_rootTraceId != 0` / `getTraceId() != 0`, and currently no messages reaching the network carry a nonzero trace ID.

**Root cause:** SLICC protocol state machines construct new protocol messages (e.g., `RequestMsg`, `ResponseMsg`) at emission sites rather than cloning the original `RubyRequest`. These fresh messages have `m_rootTraceId == 0` by default. The trace ID set on the `RubyRequest` in `Sequencer::issueRequest()` is consumed by the L1 controller but never copied into outgoing protocol messages.

**The propagation gap in detail:**
1. `Sequencer::issueRequest()` creates a `RubyRequest` with `m_rootTraceId` set ✓
2. `RubyRequest` is enqueued into the mandatory queue → `enqueue` event fires ✓
3. L1 controller dequeues the `RubyRequest`, processes it via SLICC state machine
4. SLICC code constructs a **new** `RequestMsg` (not a clone) to send to the directory
5. This new `RequestMsg` has `m_rootTraceId == 0` ✗
6. When this message reaches `NetworkInterface::flitisizeMessage()`, the zero check skips flit creation

**Approaches to fix (in order of preference):**

1. **SLICC compiler change:** Modify the SLICC `enqueue` statement to auto-propagate `m_rootTraceId` from the triggering message to the outgoing message. This is protocol-agnostic but requires changes to `src/mem/slicc/ast/EnqueueStatementAST.py` and the generated C++ code. The SLICC enqueue statement already has access to the triggering message via `in_msg_ptr` — the generated code could insert `out_msg->setRootTraceId(in_msg_ptr->getRootTraceId())` after message construction.

2. **Per-protocol SLICC annotation:** Add explicit `out_msg.m_rootTraceId := in_msg.m_rootTraceId;` lines at key emission sites in `.sm` files. Start with MI_example, then MESI Two Level. Requires manual work per protocol but no compiler changes.

3. **Controller-level hook:** Add a hook in `AbstractController` that intercepts outgoing messages and copies `m_rootTraceId` from the most recently dequeued message on the same address. Heuristic but protocol-agnostic.

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
