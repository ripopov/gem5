# Plan: Integrate FTR Transaction Tracing into gem5 and Ruby

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

`TraceContext` is the durable origin metadata attached to a `Request`.

Recommended fields are:

- `trace_id`
- `root_trace_id`
- `parent_trace_id`
- `origin_tick`
- `origin_simobject`
- `requestor_id`
- `context_id`
- `task_id`
- `stream_id`
- `substream_id`
- `pc`
- `inst_seq_num`

For memory requests, `trace_id == root_trace_id` at creation.

For future O3 instruction tracing, the instruction transaction would become the root, and the memory request could then become a child of the instruction transaction.

That is why the model must support `root_trace_id` and `parent_trace_id` from day one.

#### 3. `TransactionNode`

The recorder should internally manage transaction nodes rather than a flat event stream only.

Recommended node kinds are:

1. `MemoryRequest`
2. `RubyMessage`
3. `NetworkPacket`
4. `Flit`
5. Future: `Instruction`, `PipelineStageToken`, `DMARequest`, `Interrupt`

Each node has:

- globally unique `trace_id`
- `root_trace_id`
- optional `parent_trace_id`
- `kind`
- `begin_tick`
- optional `end_tick`
- `creation_object`
- `retirement_object`
- static attributes map
- status

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

## ID Model

Use one global 64-bit monotonic ID space for all transaction nodes.

Do not use separate ID spaces for requests, messages, and flits.

Separate spaces make future cross-subsystem joins harder and create unnecessary complexity.

Use explicit `kind` fields instead.

Recommended correlation rules are:

1. CPU request entering Ruby creates root `MemoryRequest` node.
2. If Ruby creates a protocol message that should be traced as its own lifetime, create child `RubyMessage` node.
3. If the NI clones a multicast message into unicast packets, create a child `NetworkPacket` node per clone.
4. If a packet becomes multiple flits, create a child `Flit` node per flit.
5. Retirement of a parent does not automatically imply retirement of live children.
6. All children retain the same `root_trace_id`.

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

Create the root transaction when Ruby accepts a CPU timing request in `RubyPort::MemResponsePort::recvTimingReq()`.

That is the earliest common point for CPU-originated timing requests entering Ruby.

However, the transaction should only be committed as live once the request is accepted rather than merely observed.

If a retry path causes the request not to enter Ruby yet, the trace must not leak a fake live transaction.

The practical rule is:

1. Allocate or look up a pending trace context on packet arrival.
2. Finalize root transaction creation only once `Sequencer::makeRequest()` accepts the request.
3. If Ruby rejects the request for retry, keep no committed live transaction, or mark it as a short `Rejected` event only if desired.

### Recommended Anchor For Durable Metadata

Attach `TraceContext` to `pkt->req`, not to `Packet`, `RubyRequest`, or `SequencerRequest`.

`Packet` is too transport-local.

`RubyRequest` does not survive the full Ruby plus network lifetime.

`SequencerRequest` is useful for outstanding bookkeeping but is Ruby-specific.

`Request` is the right durability layer.

### Outstanding Request Tracking

The recorder should maintain a lightweight live-node table.

For Ruby root requests, key it by `trace_id` and secondarily by the owning `Request*` while the request is in flight.

For sequencer-side correlation, it is reasonable to cache trace metadata in `SequencerRequest` too, but that should be an optimization, not the single source of truth.

## Message And Flit Child Transactions

### Ruby Message Nodes

Not every internal message must become a transaction node in the first patch.

But the design should support it cleanly.

The recommended policy is:

1. Root node is always `MemoryRequest`.
2. Protocol and network-visible messages may create `RubyMessage` children when they first become externally visible on a `MessageBuffer` that feeds the network or a peer controller.
3. Internal queue motion may be emitted as events on the root node if no separate message node exists yet.

This policy avoids requiring a full protocol-wide SLICC metadata retrofit on day one.

### Network Packet Nodes

At `NetworkInterface::flitisizeMessage()`, create a `NetworkPacket` child for each unicast message instance injected into the network.

This is the right split point because multicast cloning and packet sizing both become explicit there.

Record static attributes such as:

- parent trace ID
- vnet
- virtual channel when assigned
- message size in bytes
- number of flits
- source NI name
- destination node or route info
- Garnet packet ID

### Flit Nodes

Also in `NetworkInterface::flitisizeMessage()`, create a `Flit` child node for each constructed flit.

Each flit node should carry:

- parent packet trace ID
- flit index
- flit type
- width
- vnet
- VC if known
- route metadata if available

Child creation must be explicit in the trace rather than inferred offline from packet size and link width.

That explicitness matters for correctness once multicast, bridges, or SerDes paths enter the picture.

## Timestamp Capture Model

### General Rule

Record timestamps as events at subsystem boundaries and meaningful internal stages.

Do not try to stamp every line of code.

Choose hook points that correspond to ownership changes, resource contention points, and visible state transitions.

### Required Ruby Timestamps

For the first version, record at least these timestamps on the root `MemoryRequest` node.

1. `RubyAccepted`
2. `SequencerInserted`
3. `RubyIssued`
4. `SequencerCallback`
5. `RubyResponseSent`
6. `Retired`

### Required Queue Timestamps

For message-carrying nodes or the root node if message nodes are not created yet, record:

1. `Enqueue` at `MessageBuffer::enqueue()`
2. `Dequeue` at `MessageBuffer::dequeue()`

This is a high-value low-intrusion hook because `MessageBuffer` already centralizes queue timing.

### Required Garnet Timestamps

For `Flit` nodes, record at least:

1. `Created` at flit construction
2. `Inject` when scheduled onto the outgoing NI path
3. `RouterArrive` at `InputUnit::wakeup()`
4. `SwitchAlloc` at `SwitchAllocator`
5. `SwitchTraverse` at `CrossbarSwitch`
6. `LinkTraverse` at `NetworkLink::wakeup()`
7. `Eject` at destination NI arrival
8. `Destroy` when the flit retires from NI processing

These timestamps align with Garnet's real pipeline stages and give enough fidelity to explain contention and routing delay.

## Instrumentation Mechanisms

### Use Probes Where Practical

The existing probe framework is the right long-term way to publish trace-relevant events.

It already supports typed listeners, clean lifecycle registration, and object-local ownership.

For new generic trace points, prefer `ProbePointArg<T>` with small typed structs such as `TraceStamp` or `TraceNodeCreate`.

### Allow Direct Recorder Calls At High-Value Choke Points

Some Ruby and Garnet paths are central enough that a direct recorder call is acceptable initially.

Examples are `MessageBuffer::enqueue()` and `NetworkInterface::flitisizeMessage()`.

The plan should not block progress on a full probe rollout before tracing becomes useful.

The preferred evolution path is:

1. Start with a few direct hooks at choke points.
2. Wrap them behind tiny helper functions.
3. Later convert those helpers to emit probes plus recorder listeners.

This keeps the first patch realistic while preserving a clean target architecture.

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

For root memory requests, static attributes should include at least:

1. physical address if valid
2. line address if known
3. request size
4. command or Ruby request type
5. requestor ID
6. context ID
7. task ID
8. stream ID and substream ID if present
9. PC if present
10. instruction sequence number if present
11. secure, prefetch, atomic, LLSC, HTM, and TLBI related flags where applicable

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

## Recommended Hook Points

### Root Memory Request Lifecycle

1. `RubyPort::MemResponsePort::recvTimingReq()`
2. `Sequencer::insertRequest()`
3. `Sequencer::issueRequest()`
4. `Sequencer::readCallback()`
5. `Sequencer::writeCallback()`
6. `Sequencer::atomicCallback()`
7. `Sequencer::unaddressedCallback()`
8. `RubyPort::ruby_hit_callback()`
9. `RubyPort::MemResponsePort::hitCallback()`

### Queueing And Message Motion

1. `MessageBuffer::enqueue()`
2. `MessageBuffer::dequeue()`

### Garnet Packet And Flit Lifecycle

1. `NetworkInterface::flitisizeMessage()`
2. `NetworkInterface::scheduleFlit()`
3. `InputUnit::wakeup()`
4. `SwitchAllocator::wakeup()`
5. `CrossbarSwitch::wakeup()`
6. `OutputUnit::insert_flit()`
7. `NetworkLink::wakeup()`
8. destination-side `NetworkInterface::wakeup()`

These points are concentrated enough to be practical and semantically rich enough to explain most observed latency.

## Ruby Protocol Considerations

The design must work even before every SLICC protocol message is extended with explicit trace metadata.

That implies a staged approach.

Stage 1 should rely on root request tracing plus queue and flit events.

Stage 2 can add proper `RubyMessage` child nodes for protocol messages that matter most.

If message metadata must survive message cloning naturally across all protocols, the eventual clean solution is to add trace fields to message definitions or to generated message base support.

That is valuable, but it should not block the first usable version.

## Future O3 Extension Path

This architecture is intentionally not Ruby-specific.

Later, an O3 instruction transaction can be created at fetch or rename using the same recorder.

That instruction transaction would then parent child nodes such as:

1. decode token
2. LSQ memory request
3. cache miss request
4. Ruby message tree

The same event vocabulary remains useful.

`Created`, `Accepted`, `Enqueue`, `Dequeue`, `Issued`, `Callback`, and `Retire` all map naturally onto pipeline stages.

This is why the core framework should live under a generic simulation tracing location rather than under a Ruby-only directory.

## Implementation Plan

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

### Step 2. Add `TraceContext` Extension To `Request`

Define a request extension type carrying root trace metadata.

Add helpers to:

1. check whether a request already has trace context
2. create and attach one
3. retrieve root trace ID cheaply from any later path holding a `RequestPtr`

This step is the key future-proofing move.

### Step 3. Add Root Ruby Request Instrumentation

Instrument:

1. `RubyPort::MemResponsePort::recvTimingReq()`
2. `Sequencer::insertRequest()`
3. `Sequencer::issueRequest()`
4. completion and hit callback paths

Goal:

1. create root `MemoryRequest` node
2. record request attributes
3. stamp acceptance, issue, callback, and retire events

At the end of this step, every Ruby request should already have a useful lifetime trace even before network details are added.

### Step 4. Add Queue-Level Timestamps

Instrument `MessageBuffer::enqueue()` and `MessageBuffer::dequeue()`.

Record queue events on the most specific known node.

If a message child node exists, stamp that node.

Otherwise stamp the root request node with queue object and event attributes.

This gives immediate value with very little code disruption.

### Step 5. Add Garnet Packet And Flit Children

Instrument `NetworkInterface::flitisizeMessage()`.

Create:

1. one `NetworkPacket` child per injected unicast message instance
2. one `Flit` child per flit

Record parent-child relations explicitly.

Stamp injection and per-flit creation immediately.

### Step 6. Add Router And Link Stage Timestamps

Instrument the Garnet router and link choke points.

Record events for arrival, arbitration, switch traversal, link traversal, and ejection.

Keep the event payloads small and stage oriented.

Do not dump large repeated route structures on every event.

### Step 7. Add Optional Probe Wrappers

Once the direct hook set is stable, add typed trace probes for the same semantic events.

This step makes the framework more extensible for future subsystems and external listeners.

### Step 8. Add Protocol-Aware Message Nodes Where Worthwhile

After the generic path is proven useful, add richer `RubyMessage` child-node creation at key protocol emission sites.

Start with one commonly used protocol such as MESI Two Level.

Use that work to decide whether message-level trace metadata belongs in SLICC-generated message classes or in a narrow side-table bridge.

### Step 9. Add Validation And Documentation

Add tests that verify:

1. monotonic and unique ID allocation
2. correct parent-child relationships
3. retirement of root and child nodes
4. stable dotted object names
5. correct event ordering for a simple Ruby request
6. flit split counts versus packet size and link width

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
3. queue events appear in plausible order
4. flit child count matches expected packet decomposition

### Human Inspection

Open the trace in the intended FTR tooling and confirm that dotted hierarchy names match the SimObject design hierarchy and that request trees are easy to follow.

## Risks And Mitigations

### Risk: Too Much Protocol Coupling

Mitigation: make root request tracing independent of message-level SLICC changes.

### Risk: High Runtime Overhead

Mitigation: make message nodes and fine-grain router stamps configurable.

Start with root request events always on and deep network stamps optional.

### Risk: Pointer-Lifetime Correlation Bugs

Mitigation: put durable semantics on `Request` extension data and recorder IDs, not on transient pointers alone.

### Risk: Trace Explosion On Large Networks

Mitigation: support event filtering by node kind, object path prefix, vnet, or event kind.

### Risk: Hard To Extend To O3 Later

Mitigation: keep the recorder generic, keep IDs global, and keep event vocabulary subsystem-neutral.

## Recommended Final Shape

The best transaction trace for gem5 is a generic, hierarchical, event-driven transaction recorder whose first root objects are Ruby memory requests and whose first child objects are network packets and flits.

The durable root identity should live on `Request` through an extension object.

The runtime recorder should be a `SimObject`.

Object names written to FTR should always reuse canonical SimObject dotted hierarchy names.

Ruby should start with a small number of high-value choke points.

Garnet should explicitly create child nodes during flitisization.

That combination gives a useful first implementation now and a clean path to future O3 instruction tracing later.

## If You Remember One Thing

Do not design FTR as a Ruby-specific log.

Design it as a generic transaction graph for gem5, anchored in `Request`, named by `SimObject`, and instantiated first for Ruby request to flit lifetimes.
