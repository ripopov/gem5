# Payload Journey: From a RISC-V Load to a Network Flit and DRAM

**Audience:** Engineers studying gem5's Ruby memory system who want to understand
exactly what data travels through the Garnet network-on-chip and to the memory
controller — how a CPU instruction becomes protocol messages, then flits, and
how the directory controller bridges the NoC to DRAM.

---

## Table of Contents

1. [The Motivating Question](#1-the-motivating-question)
2. [End-to-End Overview: Six Types, Five Boundaries](#2-end-to-end-overview-six-types-five-boundaries)
3. [Stage 1 — Instruction Execution Creates a Request](#3-stage-1--instruction-execution-creates-a-request)
4. [Stage 2 — Request Is Wrapped in a Packet](#4-stage-2--request-is-wrapped-in-a-packet)
5. [Stage 3 — Packet Enters Ruby via the Sequencer](#5-stage-3--packet-enters-ruby-via-the-sequencer)
6. [Stage 4 — SLICC State Machine Creates a Protocol Message](#6-stage-4--slicc-state-machine-creates-a-protocol-message)
7. [Protocol Messages in Depth](#7-protocol-messages-in-depth)
8. [Size Classification: Control vs. Data](#8-size-classification-control-vs-data)
9. [Stage 5 — Flitization: Messages Become Flits](#9-stage-5--flitization-messages-become-flits)
10. [What the Router Sees](#10-what-the-router-sees)
11. [Reassembly at the Destination](#11-reassembly-at-the-destination)
12. [The Response Path: Flit Back to CPU](#12-the-response-path-flit-back-to-cpu)
    - [12.5. The Directory-to-Memory-Controller Path](#125-the-directory-to-memory-controller-path)
13. [Virtual Networks: Traffic Class Separation](#13-virtual-networks-traffic-class-separation)
14. [Multicast-to-Unicast Conversion](#14-multicast-to-unicast-conversion)
15. [Serialization and Deserialization (HeteroGarnet)](#15-serialization-and-deserialization-heterogarnet)
16. [Functional Access: Bypassing the Network](#16-functional-access-bypassing-the-network)
17. [Statistics: What the NI Measures](#17-statistics-what-the-ni-measures)
18. [Worked Example: GETX Request (8 Bytes, 1 Flit)](#18-worked-example-getx-request-8-bytes-1-flit)
19. [Worked Example: Data Response (72 Bytes, 5 Flits)](#19-worked-example-data-response-72-bytes-5-flits)
20. [Protocol Comparison: Message Types Across Protocols](#20-protocol-comparison-message-types-across-protocols)
21. [Common Misconceptions](#21-common-misconceptions)
22. [Key Ideas](#22-key-ideas)

---

## 1. The Motivating Question

You are staring at a Garnet router pipeline — five stages (IB → RC → SA → ST → LT), virtual channels,
credit-based flow control — and you wonder:
*what is actually inside those flits?*
Is it raw bytes of a cache line?
A serialized C++ object?
Some kind of header plus payload?

And stepping further back: when a RISC-V `lw` instruction executes on a CPU,
what happens to that load request as it travels through caches, coherence
controllers, and the network-on-chip?
How many times is it re-packaged, and what is lost at each step?

This chapter answers both questions by tracing the complete path — from
instruction to flit and back — then zooming into each layer in detail.

---

## 2. End-to-End Overview: Six Types, Five Boundaries

There are **six distinct payload types** and **five conversion boundaries**
between a CPU instruction and a Garnet flit.
Each boundary transforms the representation to serve a different abstraction
layer's needs.

```mermaid
graph LR
    subgraph CPU["CPU core"]
        A["<b>StaticInst + ExecContext</b><br/><i>Instruction execution</i>"]
    end
    subgraph Ruby["Ruby memory system"]
        B["<b>Request</b><br/><i>Memory operation</i>"]
        C["<b>Packet</b><br/><i>Transport envelope</i>"]
        D["<b>SLICC Protocol</b><br/><i>Coherence state-machine action</i>"]
    end
    subgraph Garnet["Garnet NoC"]
        E["<b>RequestMsg</b><br/>(or Response)<br/><i>Network message</i>"]
        F["<b>flit</b><br/><i>Link timing model</i>"]
    end

    A -- "①" --> B
    B -- "②" --> C
    C -- "③" --> D
    D -- "④" --> E
    E -- "⑤" --> F
```

Before diving into each stage, here is the summary:

1. [`StaticInst`](../../src/cpu/static_inst.hh#L88) + [`ExecContext`](../../src/cpu/exec_context.hh#L71) — Instruction execution: StaticInst provides the recipe (opcode, which registers), ExecContext provides runtime register values for effective address computation
2. [`Request`](../../src/mem/request.hh#L97) — Memory operation: physical address, size, flags, requestor
3. [`Packet`](../../src/mem/packet.hh#L294) — Transport envelope: MemCmd, data pointer, SenderState for return routing
4. [`RubyRequest`](../../src/mem/ruby/slicc_interface/RubyRequest.hh#L61) — Ruby bridge: maps Packet semantics to RubyRequestType, carries Packet back-pointer
5. [`RequestMsg`](../../src/mem/ruby/protocol/MESI_Two_Level-msg.sm#L66) — Coherence message: GETS/GETX/INV + address + destination; no Packet reference
6. [`flit`](../../src/mem/ruby/network/garnet/flit.hh#L50) — Network timing unit: carries RequestMsg by pointer, adds routing and VC metadata

**The critical insight: the [`Packet`](../../src/mem/packet.hh#L294) never enters the network.**
The SLICC state machine (boundary ④) is where CPU-world semantics are
translated into coherence-world semantics.
Everything the network carries is a coherence message — an address, a type,
a requestor, and a destination — wrapped in flits for timing-accurate
transport.

### The Directory-to-Memory Branch

The chain above covers the CPU → NoC path.
But when a coherence request reaches the **Directory controller** and the data
is not cached, the directory must fetch from DRAM.
This creates a **branch off the main chain** with two additional payload types:

```mermaid
graph LR
    subgraph Garnet["Garnet NoC"]
        A["<b>RequestMsg</b><br/>(GETS)"]
        F["<b>flit</b>"]
    end
    subgraph Dir["Directory controller"]
        B["<b>MemoryMsg</b><br/><i>Memory request</i>"]
        C["<b>Packet</b><br/><i>Re-created</i>"]
    end
    subgraph DRAM["Memory controller"]
        D["<b>MemPacket</b><br/><i>DRAM scheduling</i>"]
    end

    F -- "arrive at NI" --> A
    A -- "⑥ Directory action" --> B
    B -- "⑦ serviceMemoryQueue" --> C
    C -- "RequestPort" --> D
```

The directory acts as a **gateway**: it has MessageBuffers connected to the
NoC on one side, and a standard `RequestPort` connected directly to `MemCtrl`
on the other.
The memory controller is **not** on the Garnet NoC — it sees only `Packet`
objects, the same interface used by Classic caches.
See [Section 12.5](#125-the-directory-to-memory-controller-path) for the full
walkthrough.

### What Each Conversion Discards and Adds

Most conversions are **lossy projections** — they keep what the next layer needs
and drop what it doesn't (though boundaries ② and ⑤ are lossless wrappers).

| Boundary | What Is Kept | What Is Added | What Is Lost / Left Behind |
|----------|-------------|---------------|---------------------------|
| ① [`StaticInst`](../../src/cpu/static_inst.hh#L88) + [`ExecContext`](../../src/cpu/exec_context.hh#L71) → [`Request`](../../src/mem/request.hh#L97) | Effective address (from registers + immediate), size, access mode | `_requestorId`, `_pc`, `_contextId`, flag encoding | Register operands, instruction encoding, pipeline state |
| ② [`Request`](../../src/mem/request.hh#L97) → [`Packet`](../../src/mem/packet.hh#L294) | All of Request (by pointer) | `MemCmd`, `data` pointer, `SenderState` stack | Nothing lost — Packet holds `RequestPtr` |
| ③ [`Packet`](../../src/mem/packet.hh#L294) → [`RubyRequest`](../../src/mem/ruby/slicc_interface/RubyRequest.hh#L61) | Address, size, access type, PC, prefetch | `m_LineAddress` (aligned), `RubyRequestType`, `m_pkt` back-pointer | `MemCmd` mapped to `RubyRequestType` (mostly a rename); SenderState stays in Packet |
| ④ [`RubyRequest`](../../src/mem/ruby/slicc_interface/RubyRequest.hh#L61) → [`RequestMsg`](../../src/mem/ruby/protocol/MESI_Two_Level-msg.sm#L66) | Address, access mode, prefetch | `CoherenceRequestType`, `Destination`, `Requestor`, `MessageSizeType` | **Packet pointer**, PC, access size, thread context — all stay in Sequencer's request table |
| ⑤ [`RequestMsg`](../../src/mem/ruby/protocol/MESI_Two_Level-msg.sm#L66) → [`flit`](../../src/mem/ruby/network/garnet/flit.hh#L50) | Entire RequestMsg (by pointer) | `RouteInfo`, `m_vc`, `m_vnet`, `flit_type`, pipeline stage | Nothing lost — flit holds `MsgPtr` |

The most notable boundary is **④**: this is where the CPU-world information
(the [`Packet`](../../src/mem/packet.hh#L294), the PC, the thread context) is left behind.
From this point forward, the network carries only coherence protocol semantics.

The rest of this chapter walks through each stage in detail.

---

## 3. Stage 1 — Instruction Execution Creates a Request

This is where the journey begins: inside the CPU pipeline.
A RISC-V instruction like `lw x5, 0(x10)` has been fetched, decoded, and is now executing.
The CPU knows what operation to perform (a 4-byte load), and it has read the base register (`x10`) to compute an effective address.
But the CPU pipeline deals in registers, opcodes, and execution contexts — it has no concept of cache lines, coherence states, or network routing.
It must distill the instruction down to a pure memory operation: "read 4 bytes from physical address X."
That distilled representation is the `Request` object — a separation between the instruction world (which register to write, what PC we're at, what privilege mode we're in) and the memory world (what address, how many bytes, what kind of access).

The ISA decoder template calls a CPU memory-access method:

**ISA template** ([`src/arch/riscv/isa/formats/mem.isa:149`](../../src/arch/riscv/isa/formats/mem.isa#L149)):

```cpp
// LoadInitiateAcc template (simplified)
Fault %(class_name)s::initiateAcc(ExecContext *xc, ...) const
{
    Addr EA = Rs1 + offset;   // effective address from register + immediate
    return initiateMemRead(xc, traceData, EA, Mem, memAccessFlags);
}
```

For a store, the template calls `writeMemTimingLE()` instead.
These delegate to the CPU model.

**TimingSimpleCPU** ([`src/cpu/simple/timing.cc:468`](../../src/cpu/simple/timing.cc#L468)):

```cpp
RequestPtr req = std::make_shared<Request>(
    addr, size, flags, dataRequestorId(), pc, thread->contextId());
```

### What Is a Request?

A [`Request`](../../src/mem/request.hh#L97) captures the *memory operation
semantics* — what the CPU wants to do, independent of how it will be
transported:

```
┌─────────────────────────────────────────────────┐
│                   Request                       │
├─────────────────┬───────────────────────────────┤
│ _paddr          │ Physical address (after MMU)  │
│ _vaddr          │ Virtual address               │
│ _size           │ Access size (1, 2, 4, 8 bytes)│
│ _flags          │ INST_FETCH, UNCACHEABLE,      │
│                 │ LOCKED_RMW, LLSC, PREFETCH,...│
│ _requestorId    │ Who issued this (CPU0, DMA,..)│
│ _pc             │ Program counter of the insn   │
│ _contextId      │ Hardware thread ID            │
│ _byteEnable     │ Byte-enable mask for writes   │
│ atomicOpFunctor │ Lambda for AMO operations     │
└─────────────────┴───────────────────────────────┘
```

`RequestPtr` is a `std::shared_ptr<Request>` — the `Request` outlives any
single `Packet` built from it.

> **Key point:** The `Request` knows *what* memory operation to perform but
> nothing about *how* to transport it through the memory hierarchy.

---

## 4. Stage 2 — Request Is Wrapped in a Packet

The `Request` captures *what* the CPU wants from memory, but it says nothing about *how* to move that request through the interconnect.
gem5's memory system is built on a port-based architecture where components communicate by sending `Packet` objects through connected ports — every cache, crossbar, and memory controller speaks this common language.
So the CPU must wrap its `Request` inside a `Packet` before anything else in the memory hierarchy can process it.
The `Packet` adds transport machinery on top of the `Request`: a command enum (`MemCmd`), a data pointer, and a `SenderState` stack for routing responses back.
The command is derived from the `Request`'s flags — a locked load becomes `LoadLockedReq`, a prefetch becomes `SoftPFReq`, and a plain load becomes `ReadReq`.

**Packet creation** ([`src/cpu/simple/timing.cc:415`](../../src/cpu/simple/timing.cc#L415)):

```cpp
PacketPtr buildPacket(const RequestPtr &req, bool read) {
    return read ? Packet::createRead(req) : Packet::createWrite(req);
}
```

**`Packet::createRead()`** ([`src/mem/packet.hh:1037`](../../src/mem/packet.hh#L1037)):

```cpp
static PacketPtr createRead(const RequestPtr &req) {
    return new Packet(req, makeReadCmd(req));
}
```

**`makeReadCmd()`** ([`src/mem/packet.hh:993`](../../src/mem/packet.hh#L993))
inspects the Request flags to choose the right command:

```cpp
static MemCmd makeReadCmd(const RequestPtr &req) {
    if (req->isHTMCmd())         return MemCmd::HTMReq;
    else if (req->isLLSC())      return MemCmd::LoadLockedReq;
    else if (req->isPrefetch())  return MemCmd::SoftPFReq;
    else if (req->isLockedRMW()) return MemCmd::LockedRMWReadReq;
    else                         return MemCmd::ReadReq;
}
```

### What Is a Packet?

```
┌─────────────────────────────────────────────────┐
│                    Packet                       │
├─────────────────┬───────────────────────────────┤
│ cmd             │ MemCmd (ReadReq, WriteReq,    │
│                 │   LoadLockedReq, SwapReq, ..) │
│ req             │ RequestPtr → original Request │
│ addr            │ Physical address (from req)   │
│ size            │ Access size (from req)        │
│ data            │ Pointer to payload bytes      │
│                 │   (for writes and responses)  │
│ senderState     │ Stack of opaque state objects │
│                 │   (for routing responses back)│
│ headerDelay     │ Accumulated interconnect delay│
│ payloadDelay    │ Pipeline serialization delay  │
└─────────────────┴───────────────────────────────┘
```

The `Packet` adds three things the `Request` didn't have:
1. **`MemCmd`** — a transport-level command enum (`ReadReq`, `WriteReq`,
   `WritebackDirty`, etc.) derived from the Request flags
2. **`data` pointer** — for writes, points to the bytes to store;
   for read responses, points to the returned data
3. **`SenderState` stack** — an opaque linked list that lets each layer
   push its own bookkeeping so responses can be routed back

The `Packet` is the universal currency of gem5's memory system.
Every component — CPU, cache, crossbar, memory controller — speaks `Packet`.

---

## 5. Stage 3 — Packet Enters Ruby via the Sequencer

This stage is where the `Packet` crosses the boundary between gem5's generic memory system and Ruby's coherence protocol world.
The `RubyPort` acts as an adapter: it receives the `Packet` from the CPU's port interface (the same interface a Classic cache would use) and hands it to the `Sequencer`, which is Ruby's front door.
The Sequencer maps the `Packet`'s `MemCmd` to a `RubyRequestType` — for the common cases (`ReadReq` → `LD`, `WriteReq` → `ST`) this is little more than a rename across a subsystem boundary, but `MemCmd` is a much larger enum (~60 values covering writebacks, clean evictions, cache maintenance, and other transport-level operations that never come from the CPU), so the mapping also filters down to just the request types that the SLICC state machines care about (loads, stores, atomics, LL/SC).
It then creates a `RubyRequest` message and enqueues it into the "mandatory queue" — the `MessageBuffer` that feeds the L1 cache controller's SLICC state machine.

**RubyPort entry** ([`RubyPort.cc:293`](../../src/mem/ruby/system/RubyPort.cc#L293)):

```cpp
bool RubyPort::MemResponsePort::recvTimingReq(PacketPtr pkt)
{
    // Push sender state so we can route the response back
    pkt->pushSenderState(new SenderState(this));

    // Delegate to the Sequencer
    RequestStatus requestStatus = owner.makeRequest(pkt);
    ...
}
```

The [`Sequencer`](../../src/mem/ruby/system/Sequencer.cc)
([`Sequencer.cc:949`](../../src/mem/ruby/system/Sequencer.cc#L949))
maps the Packet's `MemCmd` to a `RubyRequestType`:

| Packet MemCmd | RubyRequestType |
|---------------|-----------------|
| `ReadReq` | `RubyRequestType_LD` |
| `ReadReq` (ifetch) | `RubyRequestType_IFETCH` |
| `WriteReq` | `RubyRequestType_ST` |
| `LoadLockedReq` | `RubyRequestType_Load_Linked` |
| `StoreCondReq` | `RubyRequestType_Store_Conditional` |
| `SwapReq` | `RubyRequestType_ATOMIC_RETURN` |

Then `issueRequest()` ([`Sequencer.cc:1086`](../../src/mem/ruby/system/Sequencer.cc#L1086))
creates a [`RubyRequest`](../../src/mem/ruby/slicc_interface/RubyRequest.hh#L61)
message:

```cpp
msg = std::make_shared<RubyRequest>(
    clockEdge(), blk_size, m_ruby_system,
    pkt->getAddr(), pkt->getSize(),
    pc, secondary_type, RubyAccessMode_Supervisor,
    pkt, PrefetchBit_No, proc_id, core_id);
```

### What Is a RubyRequest?

`RubyRequest` inherits from `Message` (the base class for all Ruby messages).
It is a bridge between the Packet world and the SLICC protocol world:

```
┌─────────────────────────────────────────────────┐
│                 RubyRequest                     │
│            (extends Message)                    │
├─────────────────┬───────────────────────────────┤
│ m_PhysicalAddress│ Physical address (from pkt)  │
│ m_LineAddress   │ Cache-line-aligned address    │
│ m_Type          │ RubyRequestType (LD, ST, ..)  │
│ m_ProgramCounter│ PC (from Request)             │
│ m_AccessMode    │ Supervisor / User             │
│ m_Size          │ Access size (from pkt)        │
│ m_Prefetch      │ Prefetch indicator            │
│ m_pkt           │ PacketPtr → original Packet   │
│ m_contextId     │ Hardware thread ID            │
│ m_writeMask     │ Byte mask for atomics         │
│ m_WTData        │ DataBlock for write-through   │
└─────────────────┴───────────────────────────────┘
```

The `RubyRequest` is enqueued into the **mandatory queue** — a `MessageBuffer`
that connects the Sequencer to the L1 cache controller's SLICC state machine:

```cpp
// Sequencer.cc:1172
m_mandatory_q_ptr->enqueue(msg, clockEdge(), latency, ...);
```

> **Key point:** The `RubyRequest` still carries a back-pointer to the original
> `Packet` (`m_pkt`).
> This pointer is used later when the protocol completes the request — the
> Sequencer retrieves the `Packet`, fills in the response data, and sends it
> back to the CPU.
> But this pointer never enters the network.

---

## 6. Stage 4 — SLICC State Machine Creates a Protocol Message

The L1 cache controller is a SLICC-generated state machine: it receives the `RubyRequest` from the mandatory queue, looks up the cache line's current coherence state, and decides what to do.
If the line is already present in the right state (e.g., the cache has it in Modified and the CPU wants a store), the request can be satisfied locally with no network traffic at all.
But on a cache miss, the state machine must ask the rest of the memory system for help by creating a protocol-specific message — a `RequestMsg` for MESI, a `CHIRequestMsg` for CHI — and enqueuing it into a `MessageBuffer` connected to the network.
This is the boundary where CPU-world information (the `Packet`, the PC, the thread context) is left behind: the `RequestMsg` carries only coherence semantics, and the Sequencer retains everything else for later completion.

Here is how this looks in MESI_Two_Level.

**Mandatory queue reception** ([`MESI_Two_Level-L1cache.sm:496`](../../src/mem/ruby/protocol/MESI_Two_Level-L1cache.sm#L496)):

```slicc
in_port(mandatoryQueue_in, RubyRequest, mandatoryQueue, rank = 0) {
    if (mandatoryQueue_in.isReady(clockEdge())) {
        peek(mandatoryQueue_in, RubyRequest, block_on="LineAddress") {
            // Map RubyRequestType → protocol event
            trigger(mandatory_request_type_to_event(in_msg.Type),
                    in_msg.LineAddress, ...);
        }
    }
}
```

The mapping from `RubyRequestType` to protocol events
([`MESI_Two_Level-L1cache.sm:288`](../../src/mem/ruby/protocol/MESI_Two_Level-L1cache.sm#L288)):

```slicc
Event mandatory_request_type_to_event(RubyRequestType type) {
    if (type == RubyRequestType:LD)          return Event:Load;
    else if (type == RubyRequestType:IFETCH) return Event:Ifetch;
    else if (type == RubyRequestType:ST)     return Event:Store;
    ...
}
```

On a cache miss (e.g., state `I` + event `Load`), the state machine fires the
`a_issueGETS` action, which creates a **`RequestMsg`** — the actual coherence
protocol message:

**Action `a_issueGETS`** ([`MESI_Two_Level-L1cache.sm:586`](../../src/mem/ruby/protocol/MESI_Two_Level-L1cache.sm#L586)):

```slicc
action(a_issueGETS, "a", desc="Issue GETS") {
    peek(mandatoryQueue_in, RubyRequest) {
        enqueue(requestL1Network_out, RequestMsg, l1_request_latency) {
            out_msg.addr := address;
            out_msg.Type := CoherenceRequestType:GETS;
            out_msg.Requestor := machineID;
            out_msg.Destination.add(mapAddressToRange(address,
                MachineType:Directory, ...));
            out_msg.MessageSize := MessageSizeType:Control;
            out_msg.Prefetch := in_msg.Prefetch;
            out_msg.AccessMode := in_msg.AccessMode;
        }
    }
}
```

Similarly, a store miss fires `b_issueGETX`
([`MESI_Two_Level-L1cache.sm:657`](../../src/mem/ruby/protocol/MESI_Two_Level-L1cache.sm#L657)):

```slicc
action(b_issueGETX, "b", desc="Issue GETX") {
    peek(mandatoryQueue_in, RubyRequest) {
        enqueue(requestL1Network_out, RequestMsg, l1_request_latency) {
            out_msg.addr := address;
            out_msg.Type := CoherenceRequestType:GETX;
            out_msg.Requestor := machineID;
            out_msg.Destination.add(mapAddressToRange(address,
                MachineType:Directory, ...));
            out_msg.MessageSize := MessageSizeType:Control;
            ...
        }
    }
}
```

**This is the critical conversion.**
The `RubyRequest` (which knows about load/store semantics, the original Packet,
the PC, the access mode) is consumed by the state machine.
What comes out is a `RequestMsg` — a pure coherence message that knows only
about addresses, coherence types (GETS/GETX/INV/PUTX), requestor IDs, and
destinations.

The `RequestMsg` is enqueued into the `requestL1Network_out` `MessageBuffer`,
which is connected to the Garnet `NetworkInterface`.

> **Key point:** The `RequestMsg` does **not** carry a pointer to the original
> `Packet` or `RubyRequest`.
> The `Packet` stays in the Sequencer's request table, indexed by address.
> When the coherence response eventually arrives, the Sequencer looks up the
> address and completes the original `Packet`.
> The network never sees the `Packet`.

### The Back-Pointer Chain During Network Transit

At any point during network transit, you can follow the pointer chain from
a flit back to the protocol message, but **not** back to the original Packet:

```
flit.m_msg_ptr ──▶ RequestMsg
                      │
                      │  (no pointer to Packet or RubyRequest)
                      │
                      ╳  The Packet lives in Sequencer::m_RequestTable,
                         indexed by address, unreachable from the network.
```

This is by design.
The network is a **stateless transport** — it doesn't need to know that the
GETS for address `0x1000` originated from a `lw` instruction at PC `0x8004`
executed by thread 3.
It only needs to move the message from router 0 to router 3 with correct
timing.

---

## 7. Protocol Messages in Depth

Now that we have seen how protocol messages are created, it is worth understanding what they actually look like and how they vary across gem5's coherence protocols.
Each protocol defines its own concrete message types in SLICC, but they all inherit from a common `Message` base class that provides the minimal interface the network needs.
This means you can define entirely new protocols with new message formats, and the network will transport them without modification — it never looks beyond the base class interface.

### The Base Class

Every message that enters the Garnet network inherits from
[`Message`](../../src/mem/ruby/slicc_interface/Message.hh#L62).
This base class provides:

```cpp
// src/mem/ruby/slicc_interface/Message.hh (simplified)
class Message {
  public:
    virtual MsgPtr clone() const = 0;
    virtual const MessageSizeType& getMessageSize() const;
    virtual const NetDest& getDestination() const;
    int getVnet() const { return vnet; }

  private:
    Tick m_time;              // creation timestamp
    Tick m_LastEnqueueTime;   // when last enqueued
    Tick m_DelayedTicks;      // accumulated delay
    int incoming_link;        // which link delivered this
    int vnet;                 // virtual network assignment
};
```

The key contract: every message knows its **destination** (`NetDest`), its
**size category** (`MessageSizeType`), and its **virtual network** (`vnet`).

### Protocol-Specific Messages

Each coherence protocol defines its own concrete message types in SLICC
(`.sm` files).
The SLICC compiler generates C++ classes that inherit from `Message`.

**MESI_Two_Level** ([`MESI_Two_Level-msg.sm`](../../src/mem/ruby/protocol/MESI_Two_Level-msg.sm))
defines two message structures:

**RequestMsg** — coherence requests (GETX, GETS, INV, PUTX, etc.):

```slicc
// src/mem/ruby/protocol/MESI_Two_Level-msg.sm, line 66
structure(RequestMsg, desc="...", interface="Message") {
  Addr addr,                          // cache line physical address
  CoherenceRequestType Type,          // GETX, GETS, INV, PUTX, ...
  RubyAccessMode AccessMode,          // user/supervisor
  MachineID Requestor,                // who sent this
  NetDest Destination,                // who receives it
  MessageSizeType MessageSize,        // Control or Data (size category)
  DataBlock DataBlk,                  // 64-byte cache line (if PUTX)
  int Len,
  bool Dirty,
  PrefetchBit Prefetch,
}
```

**ResponseMsg** — coherence responses (DATA, ACK, INV, UNBLOCK, etc.):

```slicc
// src/mem/ruby/protocol/MESI_Two_Level-msg.sm, line 95
structure(ResponseMsg, desc="...", interface="Message") {
  Addr addr,                          // cache line physical address
  CoherenceResponseType Type,         // DATA, DATA_EXCLUSIVE, ACK, INV, ...
  MachineID Sender,                   // who sent the response
  NetDest Destination,                // who receives it
  DataBlock DataBlk,                  // 64-byte cache line (if data response)
  bool Dirty,
  int AckCount,                       // number of acks bundled
  MessageSizeType MessageSize,        // Control or Data
}
```

Notice that both structures carry a `DataBlock DataBlk` field, but not every
message fills it with meaningful data.
A GETX request sets `MessageSize = Request_Control` (8 bytes) even though the
`DataBlock` field exists in the C++ object — the size category determines how
the network treats the message, not the C++ struct layout.

### What Other Protocols Define

Different protocols define different message structures, but they all follow the
same pattern: fields for address, type enum, sender/destination, data block, and
a `MessageSizeType`.

| Protocol | Message Structures | Notable Fields |
|----------|-------------------|----------------|
| **MI_example** ([`MI_example-msg.sm`](../../src/mem/ruby/protocol/MI_example-msg.sm)) | RequestMsg, ResponseMsg, DMARequestMsg, DMAResponseMsg | Simplest; basic invalidation |
| **MESI_Two_Level** ([`MESI_Two_Level-msg.sm`](../../src/mem/ruby/protocol/MESI_Two_Level-msg.sm)) | RequestMsg, ResponseMsg | Standard two-level hierarchy |
| **MESI_Three_Level** ([`MESI_Three_Level-msg.sm`](../../src/mem/ruby/protocol/MESI_Three_Level-msg.sm)) | CoherenceMsg | L0-L1 private link message |
| **MOESI_CMP_token** ([`MOESI_CMP_token-msg.sm`](../../src/mem/ruby/protocol/MOESI_CMP_token-msg.sm)) | PersistentMsg, RequestMsg, ResponseMsg, DMA* | `int Tokens` field for token counting |
| **CHI** ([`chi/CHI-msg.sm`](../../src/mem/ruby/protocol/chi/CHI-msg.sm)) | CHIRequestMsg, CHIResponseMsg, CHIDataMsg | Transaction IDs, `WriteMask`, partial data |
| **Garnet_standalone** ([`Garnet_standalone-msg.sm`](../../src/mem/ruby/protocol/Garnet_standalone-msg.sm)) | RequestMsg (type=MSG only) | Synthetic traffic; no real coherence |

---

## 8. Size Classification: Control vs. Data

Before a protocol message can be broken into flits, the network needs to know how large it is — but it does not inspect the message's C++ fields to figure this out.
Instead, every message carries a `MessageSizeType` tag that the SLICC protocol assigns at creation time.
Despite a large enum of possible values, they all resolve to just two byte sizes: "control" (~8 bytes) and "data" (~72 bytes).
This is a modeling abstraction: the byte sizes represent what a real NoC would need to transfer on its physical links, not the size of the simulator's C++ objects.
A `RequestMsg` might occupy hundreds of bytes on the heap (with its `DataBlock` field, vtable pointer, and padding), but the network treats it as 8 bytes because its tag says "control."

### The MessageSizeType Enum

The enum is defined in
[`RubySlicc_Exports.sm`](../../src/mem/ruby/protocol/RubySlicc_Exports.sm#L294):

```slicc
enumeration(MessageSizeType, desc="...") {
  Control,              desc="Control Message";
  Data,                 desc="Data Message";
  Request_Control,      desc="Request";
  Reissue_Control,      desc="Reissued request";
  Response_Data,        desc="data response";
  ResponseL2hit_Data,   desc="data response";
  ResponseLocal_Data,   desc="data response";
  Response_Control,     desc="non-data response";
  Writeback_Data,       desc="Writeback data";
  Writeback_Control,    desc="Writeback control";
  Broadcast_Control,    desc="Broadcast control";
  Multicast_Control,    desc="Multicast control";
  Forwarded_Control,    desc="Forwarded control";
  Invalidate_Control,   desc="Invalidate control";
  Unblock_Control,      desc="Unblock control";
  Persistent_Control,   desc="Persistent request activation messages";
  Completion_Control,   desc="Completion messages";
}
```

### The Size Mapping

[`Network::MessageSizeType_to_int()`](../../src/mem/ruby/network/Network.cc#L166)
maps every enum value to a byte count:

```cpp
// src/mem/ruby/network/Network.cc, line 166
uint32_t
Network::MessageSizeType_to_int(MessageSizeType size_type)
{
    switch(size_type) {
      case MessageSizeType_Control:
      case MessageSizeType_Request_Control:
      case MessageSizeType_Reissue_Control:
      case MessageSizeType_Response_Control:
      case MessageSizeType_Writeback_Control:
      case MessageSizeType_Broadcast_Control:
      case MessageSizeType_Multicast_Control:
      case MessageSizeType_Forwarded_Control:
      case MessageSizeType_Invalidate_Control:
      case MessageSizeType_Unblock_Control:
      case MessageSizeType_Persistent_Control:
      case MessageSizeType_Completion_Control:
        return m_control_msg_size;         // default: 8 bytes

      case MessageSizeType_Data:
      case MessageSizeType_Response_Data:
      case MessageSizeType_ResponseLocal_Data:
      case MessageSizeType_ResponseL2hit_Data:
      case MessageSizeType_Writeback_Data:
        return m_data_msg_size;            // default: block_size + 8 bytes
    }
}
```

### Default Sizes

The defaults come from [`Network.py`](../../src/mem/ruby/network/Network.py):

```python
# src/mem/ruby/network/Network.py, lines 54-69
control_msg_size = Param.Int(8, "")
data_msg_size = Param.Int(
    Parent.block_size_bytes,     # defaults to cache line size (typically 64)
    "Size of data messages..."
)
```

And in the C++ constructor
([`Network.cc`](../../src/mem/ruby/network/Network.cc#L62)):

```cpp
m_control_msg_size = p.control_msg_size;                     // 8
m_data_msg_size = p.data_msg_size + m_control_msg_size;      // 64 + 8 = 72
```

So with default settings:

| Message Category | Byte Size | What It Represents |
|------------------|-----------|--------------------|
| Control | 8 bytes | Address + coherence command overhead |
| Data | 72 bytes | 64-byte cache line + 8-byte header |

> **Deep Dive:** The `data_msg_size` parameter represents only the data
> *payload*; the constructor adds `control_msg_size` on top to account for
> the header.
> This is why a "data message" is 72 bytes, not 64.
> If you want to model a system with 128-byte cache lines, set
> `block_size_bytes = 128`, and data messages become 128 + 8 = 136 bytes.

---

## 9. Stage 5 — Flitization: Messages Become Flits

This is the final conversion in the chain, and it is where coherence protocol messages cross into the network timing domain.
The `NetworkInterface` (NI) sits between each coherence controller and its attached Garnet router — it is the border station where protocol-level `MessageBuffer` queues meet the flit-based network.
When a protocol message is ready to send, the NI's `flitisizeMessage()` method breaks it into one or more flits based on the message's `MessageSizeType` and the link's flit width.
The NI also handles VC allocation (stalling if no free VC is available) and route computation (filling in source and destination router IDs).
What may be surprising is what flitization does *not* do — detailed below.

### Entry Point

When a coherence controller (e.g., L1 cache) wants to send a message, it
enqueues it into a [`MessageBuffer`](../../src/mem/ruby/network/MessageBuffer.hh#L74)
that is connected to the
[`NetworkInterface`](../../src/mem/ruby/network/garnet/NetworkInterface.hh#L62)
(NI).

During `GarnetNetwork::init()` ([`GarnetNetwork.cc:115`](../../src/mem/ruby/network/garnet/GarnetNetwork.cc#L115)),
each NI is connected to its node's MessageBuffers:

```cpp
for (int i = 0; i < m_nodes; i++) {
    m_nis[i]->addNode(m_toNetQueues[i], m_fromNetQueues[i]);
}
```

The `addNode()` method ([`NetworkInterface.cc:131`](../../src/mem/ruby/network/garnet/NetworkInterface.cc#L131))
stores these buffers and registers the NI as a consumer:

```cpp
void NetworkInterface::addNode(vector<MessageBuffer *>& in,
                               vector<MessageBuffer *>& out) {
    inNode_ptr = in;      // one MessageBuffer per vnet (from protocol)
    outNode_ptr = out;    // one MessageBuffer per vnet (to protocol)
    for (auto& it : in) {
        if (it != nullptr)
            it->setConsumer(this);   // NI wakes up when message arrives
    }
}
```

### The NI Wakeup

Every cycle, `NetworkInterface::wakeup()`
([`NetworkInterface.cc:192`](../../src/mem/ruby/network/garnet/NetworkInterface.cc#L192))
checks each vnet for ready messages:

```cpp
for (int vnet = 0; vnet < inNode_ptr.size(); ++vnet) {
    MessageBuffer *b = inNode_ptr[vnet];
    if (b == nullptr) continue;

    if (b->isReady(curTime)) {
        msg_ptr = b->peekMsgPtr();
        if (flitisizeMessage(msg_ptr, vnet)) {
            b->dequeue(curTime);
        }
    }
}
```

One message per vnet per cycle can be consumed.

### The Flitization Algorithm

[`NetworkInterface::flitisizeMessage()`](../../src/mem/ruby/network/garnet/NetworkInterface.cc#L374)
is where the conversion happens.
Here is the algorithm step by step:

**Step 1 — Compute flit count:**

```cpp
// NetworkInterface.cc, line 386
int num_flits = (int)divCeil(
    (float) m_net_ptr->MessageSizeType_to_int(net_msg_ptr->getMessageSize()),
    (float) oPort->bitWidth());
```

With defaults (16-byte flit width):
- Control message: $\lceil 8 / 16 \rceil = 1$ flit
- Data message: $\lceil 72 / 16 \rceil = 5$ flits

**Step 2 — Allocate a virtual channel:**

```cpp
// NetworkInterface.cc, line 397
int vc = calculateVC(vnet);
if (vc == -1) return false;   // no free VC — stall, try again next cycle
```

VC allocation uses round-robin within the vnet's VC pool.
If all VCs for this vnet are occupied, the message remains in the
`MessageBuffer` until a VC frees up.

**Step 3 — Build RouteInfo:**

```cpp
// NetworkInterface.cc, line 434
RouteInfo route;
route.vnet = vnet;
route.net_dest = new_net_msg_ptr->getDestination();
route.src_ni = m_id;
route.src_router = oPort->routerID();
route.dest_ni = destID;
route.dest_router = m_net_ptr->get_router_id(destID, vnet);
route.hops_traversed = -1;   // first router increments to 0
```

**Step 4 — Create flits:**

```cpp
// NetworkInterface.cc, line 449
for (int i = 0; i < num_flits; i++) {
    flit *fl = new flit(packet_id, i, vc, vnet, route, num_flits,
                        new_msg_ptr,                          // <-- shared_ptr!
                        m_net_ptr->MessageSizeType_to_int(
                            net_msg_ptr->getMessageSize()),
                        oPort->bitWidth(), curTick());
    fl->set_src_delay(curTick() - msg_ptr->getTime());
    niOutVcs[vc].insert(fl);
}
```

### The Critical Insight: Shared Pointer, Not Serialized Data

Look at the flit constructor ([`flit.cc:46`](../../src/mem/ruby/network/garnet/flit.cc#L46)):

```cpp
flit::flit(int packet_id, int id, int vc, int vnet, RouteInfo route,
           int size, MsgPtr msg_ptr, int MsgSize, uint32_t bWidth,
           Tick curTime)
{
    m_size = size;
    m_msg_ptr = msg_ptr;      // ← stores the shared_ptr to the SAME message
    m_enqueue_time = curTime;
    m_packet_id = packet_id;
    m_id = id;                // flit index within packet (0, 1, 2, ...)
    m_vnet = vnet;
    m_vc = vc;
    m_route = route;
    m_stage.first = I_;
    m_width = bWidth;
    msgSize = MsgSize;

    if (size == 1) { m_type = HEAD_TAIL_; return; }
    if (id == 0)             m_type = HEAD_;
    else if (id == (size-1)) m_type = TAIL_;
    else                     m_type = BODY_;
}
```

Every flit in a packet stores `m_msg_ptr` — a `std::shared_ptr<Message>`
pointing to **the same original message object**.
There is no byte-level serialization.
The HEAD flit, all BODY flits, and the TAIL flit all hold the same pointer.

**What does this mean?**

- **The flit count is purely a timing model.**
  A 5-flit data message occupies the link for 5 cycles (at 1 flit/cycle),
  correctly modeling the serialization latency of pushing 72 bytes through a
  16-byte-wide link.
- **No actual bytes are copied or packed.**
  The C++ `Message` object lives on the heap.
  Flits are lightweight wrappers that point to it.
- **Only the TAIL flit's pointer matters at the destination.**
  When the last flit arrives, the NI extracts the `MsgPtr` and delivers it to
  the protocol.
  The BODY flits' pointers are never used for delivery.

### Flit Type Assignment

The constructor assigns flit types based on position in the packet:

```
1-flit packet:   [HEAD_TAIL]

5-flit packet:   [HEAD] [BODY] [BODY] [BODY] [TAIL]
                  id=0   id=1   id=2   id=3   id=4
```

Only HEAD and HEAD_TAIL flits trigger route computation in the router.
BODY and TAIL flits follow the route established by their HEAD.

---

## 10. What the Router Sees

Once flits leave the NI and enter the Garnet router, the payload is completely opaque.
The router's pipeline stages — Input Buffering, Route Computation, Switch Allocation, Switch Traversal, and Link Traversal — operate entirely on flit-level metadata.
This section shows exactly which fields the router reads and which it ignores.

From the router's perspective, a flit is an opaque object with metadata:

```
┌──────────────────────────────────────────────────────┐
│                     flit                             │
├──────────────┬───────────────────────────────────────┤
│ m_type       │ HEAD_, BODY_, TAIL_, or HEAD_TAIL_    │
│ m_vnet       │ virtual network (e.g. 0=ctrl, 1=data) │
│ m_vc         │ virtual channel index                 │
│ m_route      │ RouteInfo {src, dest, hops}           │
│ m_outport    │ output port (set by RoutingUnit)      │
│ m_stage      │ pipeline stage + timestamp            │
│ m_packet_id  │ identifies which packet               │
│ m_id         │ flit index within packet              │
├──────────────┼───────────────────────────────────────┤
│ m_msg_ptr    │ shared_ptr<Message>  ← NEVER TOUCHED  │
│ m_width      │ flit width in bytes                   │
│ msgSize      │ original message size in bytes        │
└──────────────┴───────────────────────────────────────┘
```

The router's `InputUnit`, `SwitchAllocator`, `CrossbarSwitch`, and `OutputUnit`
**never inspect `m_msg_ptr`**.
They read only `m_type` (to know if route computation is needed), `m_vnet` and
`m_vc` (for VC management), `m_route` (for routing), and `m_stage` (for
pipeline scheduling).

This is a clean separation of concerns: the **protocol** defines what messages
mean, and the **network** provides timing-accurate transport without
understanding the payload.

---

## 11. Reassembly at the Destination

"Reassembly" is a bit of a misnomer — since no byte-level serialization happened during flitization, there are no bytes to reassemble.
What the destination NI actually does is consume flits one at a time, return credits, and wait for the TAIL flit to deliver the message to the protocol
([`NetworkInterface.cc:230-282`](../../src/mem/ruby/network/garnet/NetworkInterface.cc#L230)):

```cpp
flit *t_flit = inNetLink->consumeLink();
int vnet = t_flit->get_vnet();

if (t_flit->get_type() == TAIL_ || t_flit->get_type() == HEAD_TAIL_) {
    // End of packet — deliver message to protocol
    if (outNode_ptr[vnet]->areNSlotsAvailable(1, curTime)) {
        outNode_ptr[vnet]->enqueue(t_flit->get_msg_ptr(), curTime,
                                    cyclesToTicks(Cycles(1)), ...);
        // Send credit back with VC-free signal
        Credit *cFlit = new Credit(t_flit->get_vc(), true, curTick());
        iPort->sendCredit(cFlit);
        incrementStats(t_flit);
        delete t_flit;
    } else {
        // Protocol buffer full — stall
        iPort->m_stall_queue.push_back(t_flit);
    }
} else {
    // HEAD or BODY flit — consume, return credit, delete
    Credit *cFlit = new Credit(t_flit->get_vc(), false, curTick());
    iPort->sendCredit(cFlit);
    delete t_flit;
}
```

Key points:

1. **Only TAIL/HEAD_TAIL flits trigger message delivery.**
   The `MsgPtr` is extracted via `get_msg_ptr()` and enqueued into the
   protocol's `MessageBuffer`.
2. **HEAD and BODY flits are consumed for credits and deleted.**
   Their `MsgPtr` is never used — it exists only because every flit in the
   packet shares the same pointer.
3. **Credits distinguish VC-free vs. buffer-free.**
   TAIL flits send `Credit(vc, true)` — the VC is now free for reuse.
   HEAD/BODY flits send `Credit(vc, false)` — buffer space freed, but VC
   still in use.
4. **Back-pressure via stall queue.**
   If the protocol `MessageBuffer` is full when the TAIL arrives, the flit
   goes into a stall queue.
   When the protocol dequeues a message, a callback
   ([`dequeueCallback()`](../../src/mem/ruby/network/garnet/NetworkInterface.cc#L145))
   reschedules the NI to retry ejection next cycle.

---

## 12. The Response Path: Flit Back to CPU

With the forward path complete, we can now trace the return journey that brings data back to the processor.
The response path traverses the same payload types in reverse order, but through different code paths and with one crucial addition: the `DataBlock` carrying the actual cache line bytes.
This section also covers the sub-path through the memory controller when the data is not cached anywhere and must be fetched from DRAM.

```
Directory/L2 SLICC action
    │
    │  enqueue(responseNetwork_out, ResponseMsg) with DataBlock + MessageSize:Data
    ▼
ResponseMsg ──▶ NI flitisizeMessage() ──▶ 5 flits (72B / 16B)
    │
    │  traverse Garnet routers (5 cycles on each link)
    ▼
Destination NI ──▶ reassemble ──▶ ResponseMsg delivered to L1 controller
    │
    │  L1 SLICC action: write DataBlock into cache, signal completion
    ▼
Sequencer::readCallback(address)
    │
    │  Lookup Packet in request table by address
    │  Copy data from cache into Packet.data
    ▼
Packet (with data filled in) ──▶ RubyPort ──▶ CPU
    │
    │  Pop SenderState, route response back to requesting port
    ▼
CPU receives read response, writes data to register x5
```

The loaded data (the actual cache line bytes) travels inside the `DataBlock`
field of `ResponseMsg`.
When the L1 controller stores it in the cache, the Sequencer copies the
relevant bytes from the cache into the Packet's `data` buffer.
The Packet then travels back to the CPU via the port system, completing the
original `lw` instruction.

### 12.5. The Directory-to-Memory-Controller Path

The response path above assumed the data was already cached at the L2 or
directory level.
When the directory does **not** have a cached copy, it must fetch the cache
line from DRAM.
This path introduces two additional payload types and exits the NoC entirely.

#### Topology: The Memory Controller Is Not on the NoC

The directory controller has **two interfaces**:

1. **NoC side** — MessageBuffers connected to the Garnet `NetworkInterface`
   ([`MESI_Two_Level-dir.sm:34-39`](../../src/mem/ruby/protocol/MESI_Two_Level-dir.sm#L34)):
   `requestToDir`, `responseToDir`, `responseFromDir` carry coherence
   messages as flits through the network.
2. **Memory side** — MessageBuffers that are **not** connected to the network
   ([`MESI_Two_Level-dir.sm:41-42`](../../src/mem/ruby/protocol/MESI_Two_Level-dir.sm#L41)):
   `requestToMemory` and `responseFromMemory` carry
   [`MemoryMsg`](../../src/mem/ruby/protocol/RubySlicc_MemControl.sm#L65)
   objects, which `AbstractController` converts to `Packet` and sends through
   a direct `RequestPort` to `MemCtrl`.

In configuration
([`directory.py:53`](../../src/python/gem5/components/cachehierarchies/ruby/caches/mesi_two_level/directory.py#L53)):

```python
self.memory_out_port = port   # RequestPort wired directly to MemCtrl
```

#### Step 1: Directory Action Creates a MemoryMsg

When the directory state machine receives a GETS for a line in state I
(uncached), it transitions through state IM and fires the
`qf_queueMemoryFetchRequest` action
([`MESI_Two_Level-dir.sm:313`](../../src/mem/ruby/protocol/MESI_Two_Level-dir.sm#L313)):

```slicc
action(qf_queueMemoryFetchRequest, "qf", desc="Queue off-chip fetch request") {
    peek(requestNetwork_in, RequestMsg) {
        enqueue(memQueue_out, MemoryMsg, to_mem_ctrl_latency) {
            out_msg.addr := address;
            out_msg.Type := MemoryRequestType:MEMORY_READ;
            out_msg.Sender := in_msg.Requestor;
            out_msg.MessageSize := MessageSizeType:Request_Control;
            out_msg.Len := 0;
        }
    }
}
```

[`MemoryMsg`](../../src/mem/ruby/protocol/RubySlicc_MemControl.sm#L65) is
defined in SLICC — it carries address, `MemoryRequestType` (MEMORY_READ or
MEMORY_WB), sender, data block, and size.
It is much simpler than coherence messages: no destination set, no coherence
request type, no token count.

For writebacks, `qw_queueMemoryWBRequest`
([`MESI_Two_Level-dir.sm:325`](../../src/mem/ruby/protocol/MESI_Two_Level-dir.sm#L325))
does the same but with `MEMORY_WB` and copies the `DataBlk` from the incoming
`ResponseMsg`.

#### Step 2: MemoryMsg → Packet (boundary ⑥–⑦)

[`AbstractController::serviceMemoryQueue()`](../../src/mem/ruby/slicc_interface/AbstractController.cc#L265)
dequeues the `MemoryMsg` and converts it to a standard `Packet`:

```cpp
const MemoryMsg *mem_msg = (const MemoryMsg*)mem_queue->peek();
unsigned int req_size = m_ruby_system->getBlockSizeBytes();

RequestPtr req = std::make_shared<Request>(mem_msg->m_addr, req_size, 0, m_id);

if (mem_msg->getType() == MemoryRequestType_MEMORY_WB) {
    pkt = Packet::createWrite(req);
    pkt->allocate();
    pkt->setData(mem_msg->m_DataBlk.getData(...));
} else if (mem_msg->getType() == MemoryRequestType_MEMORY_READ) {
    pkt = Packet::createRead(req);
    pkt->dataDynamic(new uint8_t[req_size]);
}

SenderState *s = new SenderState(mem_msg->m_Sender);
pkt->pushSenderState(s);
memoryPort.sendTimingReq(pkt);
```

This is the **re-entry into gem5's standard port world**.
The `Packet` sent here is structurally identical to what a Classic cache would
send to `MemCtrl` — the memory controller has no idea it is connected to Ruby.

The `SenderState` stashes the original requestor's `MachineID` so the return
path knows who asked.

#### Step 3: MemCtrl Processes the Request

`MemCtrl` receives the `Packet` through its `ResponsePort` and wraps it in a
[`MemPacket`](../../src/mem/mem_ctrl.hh#L99) — the DRAM controller's internal
representation that decodes the physical address into rank, bank, row, and
column for scheduling.

#### Step 4: DRAM Response → MemoryMsg (return path)

When DRAM completes,
[`AbstractController::recvTimingResp()`](../../src/mem/ruby/slicc_interface/AbstractController.cc#L377)
converts the response `Packet` back into a `MemoryMsg`:

```cpp
std::shared_ptr<MemoryMsg> msg =
    std::make_shared<MemoryMsg>(clockEdge(), blk_size, m_ruby_system);
(*msg).m_addr = pkt->getAddr();
(*msg).m_Sender = m_machineID;

SenderState *s = dynamic_cast<SenderState *>(pkt->senderState);
(*msg).m_OriginalRequestorMachId = s->id;

if (pkt->isRead()) {
    (*msg).m_Type = MemoryRequestType_MEMORY_READ;
    (*msg).m_MessageSize = MessageSizeType_Response_Data;
    (*msg).m_DataBlk.setData(pkt->getPtr<uint8_t>(), 0, blk_size);
}

memRspQueue->enqueue(msg, ...);
```

The `MemoryMsg` is enqueued into `responseFromMemory`, where the directory
state machine picks it up, transitions to a stable state, and sends a
`ResponseMsg` with the `DataBlock` back through the Garnet NoC to the
requesting L1 cache.

#### Conversion Summary: Directory ↔ Memory Controller

| Boundary | From | To | Where |
|----------|------|----|-------|
| ⑥ | `RequestMsg` (coherence) | `MemoryMsg` | Directory SLICC action ([`dir.sm:313`](../../src/mem/ruby/protocol/MESI_Two_Level-dir.sm#L313)) |
| ⑦ | `MemoryMsg` | `Packet` | [`AbstractController::serviceMemoryQueue()`](../../src/mem/ruby/slicc_interface/AbstractController.cc#L265) |
| ⑦' (return) | `Packet` (DRAM response) | `MemoryMsg` | [`AbstractController::recvTimingResp()`](../../src/mem/ruby/slicc_interface/AbstractController.cc#L377) |
| ⑥' (return) | `MemoryMsg` (with data) | `ResponseMsg` | Directory SLICC action sends data through NoC |

---

## 13. Virtual Networks: Traffic Class Separation

Coherence protocols generate several distinct types of traffic — requests, forwarded snoops, responses, data transfers — and mixing them freely on the same network resources can cause head-of-line blocking and, worse, deadlock (a response blocked behind the request it is trying to satisfy).
Virtual networks (vnets) solve this by giving each traffic class its own logically independent set of resources: separate `MessageBuffer` queues at the endpoints, separate VC pools inside the routers, and separate buffer sizing.
The vnet assignment happens at message creation time in the SLICC protocol code, and the NI uses it to select the correct VC pool during flitization.

### How Vnets Are Assigned

The coherence protocol assigns the vnet when it enqueues a message.
Each protocol defines its own vnet mapping.

For MESI_Two_Level (3 vnets):
- **Vnet 0:** Requests (GETX, GETS, PUTX)
- **Vnet 1:** Forwarded requests (from directory to caches)
- **Vnet 2:** Responses (DATA, ACK, UNBLOCK)

For CHI (4 vnets, matching the CHI channel structure):
- **Vnet 0:** REQ channel (requests)
- **Vnet 1:** SNP channel (snoops)
- **Vnet 2:** RSP channel (responses)
- **Vnet 3:** DAT channel (data transfers)

### Vnet-to-Buffer-Size Mapping

Garnet classifies each vnet as either **control** or **data** based on a string
tag provided during queue registration.
In [`GarnetNetwork.cc:81`](../../src/mem/ruby/network/garnet/GarnetNetwork.cc#L81):

```cpp
for (int i = 0; i < m_virtual_networks; i++) {
    if (m_vnet_type_names[i] == "response")
        m_vnet_type[i] = DATA_VNET_;     // carries data (and ctrl) packets
    else
        m_vnet_type[i] = CTRL_VNET_;     // carries only ctrl packets
}
```

This classification controls buffer depth per VC:
- `CTRL_VNET_` → `buffers_per_ctrl_vc` (default: 1)
- `DATA_VNET_` → `buffers_per_data_vc` (default: 4)

Data VCs get deeper buffers because data messages produce more flits per
packet, and shallow buffers would cause excessive stalling.

### Vnet-to-VC Mapping

Each vnet gets its own pool of VCs.
With `vcs_per_vnet = 4` and 3 vnets:

```
Vnet 0: VCs  0,  1,  2,  3
Vnet 1: VCs  4,  5,  6,  7
Vnet 2: VCs  8,  9, 10, 11
         └── Total: 12 VCs per router port
```

The NI maps between them in
[`NetworkInterface.cc`](../../src/mem/ruby/network/garnet/NetworkInterface.cc#L617):

```cpp
int NetworkInterface::get_vnet(int vc) {
    for (int i = 0; i < m_virtual_networks; i++) {
        if (vc >= (i*m_vc_per_vnet) && vc < ((i+1)*m_vc_per_vnet))
            return i;
    }
}
```

---

## 14. Multicast-to-Unicast Conversion

Coherence protocols sometimes need to send a single message to multiple destinations — the classic example is a directory sending invalidation messages to all sharers of a cache line.
The protocol expresses this naturally by adding multiple machine IDs to the message's `NetDest` destination set.
But Garnet is a **unicast** network: each flit has exactly one destination router.
The NI bridges this gap by splitting each multicast message into separate unicast packets, one per destination
([`NetworkInterface.cc:394-428`](../../src/mem/ruby/network/garnet/NetworkInterface.cc#L394)):

```cpp
// Loop to convert all multicast messages into unicast messages
for (int ctr = 0; ctr < dest_nodes.size(); ctr++) {
    int vc = calculateVC(vnet);
    if (vc == -1) return false;        // no free VC — stall entire message

    MsgPtr new_msg_ptr = msg_ptr->clone();   // deep copy per destination
    NodeID destID = dest_nodes[ctr];

    // Rewrite destination to single node
    if (dest_nodes.size() > 1) {
        NetDest personal_dest(...);
        personal_dest.add(MachineID{type, num});
        new_net_msg_ptr->getDestination() = personal_dest;
        // Remove this dest from original so partial progress is tracked
        net_msg_ptr->getDestination().removeNetDest(personal_dest);
    }

    // Each unicast copy gets its own packet of flits
    RouteInfo route;
    route.dest_router = m_net_ptr->get_router_id(destID, vnet);
    // ... create num_flits flits with this route ...
}
```

If the message has 4 destinations and each is a control message (1 flit),
the NI produces 4 separate single-flit packets, each with its own VC, route,
and cloned message pointer.

If the NI runs out of free VCs partway through, it returns `false`, and
the remaining destinations are retried next cycle (the already-removed
destinations from `NetDest` track partial progress).

---

## 15. Serialization and Deserialization (HeteroGarnet)

Real NoC designs sometimes connect components with links of different widths — for example, a wide 32-byte link between a router and a large shared cache, and a narrower 16-byte link between routers in a mesh.
Garnet models this through `NetworkBridge` components that sit between links of different flit widths.
This feature, known as HeteroGarnet, enables modeling of heterogeneous NoC designs where different parts of the network operate at different bandwidths.

When a flit crosses a `NetworkBridge` between links of different widths, it
must be serialized (wide → narrow) or deserialized (narrow → wide).

**Serialization** ([`flit.cc:76`](../../src/mem/ruby/network/garnet/flit.cc#L76)):

```cpp
flit* flit::serialize(int ser_id, int parts, uint32_t bWidth) {
    int ratio = divCeil(m_width, bWidth);         // e.g. 32B / 16B = 2
    int new_id = (m_id * ratio) + ser_id;         // split each flit into ratio sub-flits
    int new_size = divCeil(msgSize, bWidth);       // total flits at new width
    flit *fl = new flit(m_packet_id, new_id, m_vc, m_vnet, m_route,
                        new_size, m_msg_ptr, msgSize, bWidth, m_time);
    return fl;
}
```

A single 32-byte-wide flit becomes two 16-byte-wide flits.
The `MsgPtr` is shared — again, no actual byte packing occurs.
The flit count increases to model the additional serialization cycles.

**Deserialization** ([`flit.cc:93`](../../src/mem/ruby/network/garnet/flit.cc#L93))
is the reverse: multiple narrow flits are combined into fewer wide flits.
The ratio and flit IDs are recalculated, and the flit type (HEAD/BODY/TAIL)
is re-derived from the new ID and size.

---

## 16. Functional Access: Bypassing the Network

Garnet's shared-pointer design has a practical side benefit: it makes functional accesses straightforward.
Functional accesses are simulator-level operations that bypass the timing model to inspect or modify data that is currently "in flight" through the network — for example, reading a cache line from a response message still traversing the NoC.
If the data had been serialized into actual bytes spread across multiple flits, functional access would require reassembling the flits first — but since flits are just timing wrappers around a single heap-allocated message, no reassembly is needed.

Since flits carry a `MsgPtr`, functional access simply delegates to the
message ([`flit.cc:128-140`](../../src/mem/ruby/network/garnet/flit.cc#L128)):

```cpp
bool flit::functionalRead(Packet *pkt, WriteMask &mask) {
    Message *msg = m_msg_ptr.get();
    return msg->functionalRead(pkt, mask);
}

bool flit::functionalWrite(Packet *pkt) {
    Message *msg = m_msg_ptr.get();
    return msg->functionalWrite(pkt);
}
```

The message's implementation checks whether the packet address matches.
For example, `ResponseMsg::functionalRead()` in MESI_Two_Level
([`MESI_Two_Level-msg.sm:105`](../../src/mem/ruby/protocol/MESI_Two_Level-msg.sm#L105))
only returns data for DATA-type responses:

```slicc
bool functionalRead(Packet *pkt) {
    if (Type == CoherenceResponseType:DATA ||
        Type == CoherenceResponseType:DATA_EXCLUSIVE ||
        Type == CoherenceResponseType:MEMORY_DATA) {
        return testAndRead(addr, DataBlk, pkt);
    }
    return false;
}
```

This works precisely *because* the flits carry the original message by
pointer — the `DataBlock` is still live in memory, accessible through any
of the flits that reference it.

---

## 17. Statistics: What the NI Measures

Garnet breaks network latency into three distinct components, measured at the destination NI when each flit is consumed.
These statistics appear in the simulation output under `system.ruby.network` and are useful for diagnosing whether bottlenecks are at injection, in the network fabric, or at ejection.

When a flit is consumed at the destination NI,
[`incrementStats()`](../../src/mem/ruby/network/garnet/NetworkInterface.cc#L155)
records latency and hop data:

```cpp
void NetworkInterface::incrementStats(flit *t_flit) {
    int vnet = t_flit->get_vnet();

    // Per-flit latencies
    Tick network_delay   = t_flit->get_dequeue_time()
                         - t_flit->get_enqueue_time()
                         - cyclesToTicks(Cycles(1));
    Tick src_queueing    = t_flit->get_src_delay();
    Tick dest_queueing   = curTick() - t_flit->get_dequeue_time();
    Tick total_queueing  = src_queueing + dest_queueing;

    // Per-packet latencies (only on TAIL/HEAD_TAIL)
    if (t_flit->get_type() == TAIL_ || t_flit->get_type() == HEAD_TAIL_) {
        m_net_ptr->increment_received_packets(vnet);
        m_net_ptr->increment_packet_network_latency(network_delay, vnet);
        m_net_ptr->increment_packet_queueing_latency(total_queueing, vnet);
    }

    // Hop count
    m_net_ptr->increment_total_hops(t_flit->get_route().hops_traversed);
}
```

The three latency components:

```
           src_queueing          network_delay         dest_queueing
          ◄───────────►  ◄─────────────────────────►  ◄────────────►
  ┌────────┐  ┌─────┐  ┌─────────────────────────────┐  ┌─────┐  ┌─────────┐
  │Protocol│→ │NI   │→ │  Router → Link → Router →...│→ │NI   │→ │Protocol │
  │enqueue │  │queue│  │     (hops × pipeline)       │  │eject│  │dequeue  │
  └────────┘  └─────┘  └─────────────────────────────┘  └─────┘  └─────────┘
  msg_time    enqueue_time                           dequeue_time   curTick()
```

- **Source queueing delay:** Time the message waited in the protocol
  `MessageBuffer` before the NI could inject it
  (`curTick() - msg_ptr->getTime()` at injection time).
- **Network delay:** Time from NI injection to NI arrival, minus one cycle
  (the subtracted cycle accounts for the flit's creation cycle).
- **Destination queueing delay:** Time from flit arrival at destination NI
  to actual ejection into the protocol buffer
  (non-zero if stall queue was involved).

---

## 18. Worked Example: GETX Request (8 Bytes, 1 Flit)

An L1 cache needs exclusive access to address `0x1000`.
The full path from a `sw` instruction:

**CPU:** `sw x5, 0(x10)` → Request(`_paddr=0x1000, _size=4, _flags=0`)
→ Packet(`cmd=WriteReq, req→Request, data→bytes`)

**Sequencer:** Packet → RubyRequest(`m_Type=ST, m_pkt→Packet`)
→ mandatory queue

**SLICC (state I + event Store):** RubyRequest consumed, fires `b_issueGETX`:

```
RequestMsg {
    addr = 0x1000,
    Type = GETX,
    Requestor = L1Cache-0,
    Destination = Directory-0,
    MessageSize = Request_Control,
    DataBlk = <empty>,
}
```

**Size classification:**

```
MessageSizeType_to_int(Request_Control) = m_control_msg_size = 8 bytes
```

**Flitization:**

```
num_flits = ceil(8 / 16) = 1
```

A single flit is created:

```
flit {
    m_packet_id = 42,
    m_id = 0,
    m_type = HEAD_TAIL_,
    m_vnet = 0,
    m_vc = 0,
    m_route = { src_router=0, dest_router=3, vnet=0 },
    m_msg_ptr = shared_ptr → RequestMsg above,
    m_width = 16,
    msgSize = 8,
}
```

**Router view:**
The router sees a single HEAD_TAIL flit on vnet 0.
It computes the route, allocates a switch, traverses the crossbar and output
link — all in a few cycles.
It never inspects the `RequestMsg` inside.

**Destination NI:**
The flit arrives, the NI extracts `m_msg_ptr`, enqueues the `RequestMsg` into
the directory controller's `MessageBuffer`, sends a credit back, deletes the
flit.

---

## 19. Worked Example: Data Response (72 Bytes, 5 Flits)

The L2/directory responds with the cache line for address `0x1000`.

**Protocol message:**

```
ResponseMsg {
    addr = 0x1000,
    Type = DATA_EXCLUSIVE,
    Sender = Directory-0,
    Destination = L1Cache-0,
    DataBlk = <64 bytes of data>,
    MessageSize = Response_Data,
    AckCount = 0,
}
```

**Size:**

```
MessageSizeType_to_int(Response_Data) = m_data_msg_size = 72 bytes
```

**Flitization:**

```
num_flits = ceil(72 / 16) = 5
```

Five flits are created, all pointing to the same `ResponseMsg`:

```
Flit 0: HEAD_      m_id=0  m_msg_ptr → ResponseMsg
Flit 1: BODY_      m_id=1  m_msg_ptr → ResponseMsg  (same pointer)
Flit 2: BODY_      m_id=2  m_msg_ptr → ResponseMsg  (same pointer)
Flit 3: BODY_      m_id=3  m_msg_ptr → ResponseMsg  (same pointer)
Flit 4: TAIL_      m_id=4  m_msg_ptr → ResponseMsg  (same pointer)
```

**At the destination NI:**

- Flits 0-3 (HEAD + BODY): each returns a credit, is deleted.
  The `MsgPtr` reference count stays alive because Flit 4 still holds it.
- Flit 4 (TAIL): the NI calls `outNode_ptr[vnet]->enqueue(t_flit->get_msg_ptr(), ...)`
  to deliver the `ResponseMsg` to the L1 cache controller.
  A credit with `is_free_signal = true` is sent back.
  The flit is deleted.

The 5-flit transmission correctly models that a 72-byte message takes 5 cycles
to traverse a 16-byte-wide link.
But the "data" was never serialized into bytes — the `DataBlock` lived in the
`ResponseMsg` object on the heap the entire time.

**Back at the CPU:**
The L1 controller writes the DataBlock into the cache, signals the Sequencer.
The Sequencer looks up address `0x1000` in its request table, finds the
original `Packet`, copies 4 bytes from the cache line into `Packet.data`,
and sends the Packet back through RubyPort to the CPU.
The CPU writes the loaded value into register `x5`.

---

## 20. Protocol Comparison: Message Types Across Protocols

| Protocol | Vnets | Control Messages | Data Messages | Key Difference |
|----------|-------|-----------------|---------------|----------------|
| MI_example | 5 | GETX, GETS, INV, WB_ACK, WB_NACK | PUTX, DATA, DATA_EXCLUSIVE, WRITEBACK | Simplest; DMA on separate vnets |
| MESI_Two_Level | 3 | GETX, GETS, INV, UPGRADE, ACK, UNBLOCK | DATA, DATA_EXCLUSIVE, MEMORY_DATA, WRITEBACK | Standard two-level cache hierarchy |
| MESI_Three_Level | 3 | GETX, GETS, INV, ACK, NAK, FLUSH | DATA, DATA_EXCLUSIVE, PUTX | Adds L0-L1 `CoherenceMsg` |
| MOESI_CMP_token | 6 | GETX, GETS, PERSISTENT, ACK | DATA_OWNER, DATA_SHARED, WB_* | `int Tokens` field; starvation prevention |
| CHI | 4 | ReadShared, ReadUnique, SnpClean*, Comp*, RetryAck | CompData_*, CBWrData_*, SnpRespData_* | Transaction IDs, partial data via WriteMask |
| Garnet_standalone | 3 | MSG (single type) | — | Synthetic traffic; no real coherence logic |

Despite their differences, every protocol follows the same pattern:

1. Define message structures inheriting from `Message`
2. Tag each with a `MessageSizeType` (Control or Data)
3. Assign to a vnet when sending
4. The network treats them identically — opaque payloads with size tags

---

## 21. Common Misconceptions

**"Flits contain serialized cache line bytes."**
No. Flits contain a `shared_ptr<Message>` to the original C++ object.
The flit count models serialization delay, but no byte packing occurs.
All flits in a packet point to the same message.

**"BODY flits carry different parts of the cache line."**
No. Every flit in a packet carries the same `MsgPtr`.
BODY flits exist to occupy link bandwidth for the correct number of cycles.
Only the TAIL flit's pointer is used for message delivery.

**"The router inspects the message to make routing decisions."**
No. The router reads only flit-level metadata (`m_route`, `m_vnet`, `m_vc`,
`m_type`).
The `RoutingUnit` uses `RouteInfo.dest_router` or `RouteInfo.net_dest` — both
set by the NI during flitization — not anything inside the `Message`.

**"Control messages and data messages use different flit formats."**
No. The `flit` class is the same for both.
The only difference is *how many flits* the NI creates: 1 for an 8-byte
control message vs. 5 for a 72-byte data message (with default parameters).

**"Multicast is handled by the routers."**
No. Garnet routers handle only unicast.
The source NI splits multicast messages into separate unicast packets, each
with its own cloned message, VC, and route.

**"The CPU's Packet travels through the network."**
No. The Packet stays in the Sequencer's request table.
The SLICC state machine creates a fresh `RequestMsg` with only coherence
semantics — no Packet pointer, no PC, no thread context.
The network never sees the Packet.

---

## 22. Key Ideas

1. **Six payload types span CPU to network.**
   StaticInst → Request → Packet → RubyRequest → RequestMsg → flit.
   Each conversion keeps what the next layer needs and drops the rest.

2. **The Packet never enters the network.**
   The SLICC state machine is the boundary where CPU-world information
   (Packet, PC, thread context) is left behind.
   The network carries only coherence semantics (GETS/GETX + address + destination).
   The Sequencer retains the Packet for completion when the response returns.

3. **Garnet is a timing model, not a data-movement model.**
   Flits carry shared pointers to protocol messages, not serialized bytes.

4. **Messages come from SLICC protocols.**
   Each protocol defines its own message structures (RequestMsg, ResponseMsg,
   etc.) with fields for address, type, sender, destination, and data block.

5. **Size classification is coarse-grained.**
   Every message is tagged as either Control (~8 bytes) or Data (~72 bytes).
   This tag determines flit count.

6. **The NI is the protocol-network boundary.**
   The NetworkInterface converts protocol messages to flits (flitization) and
   flits back to messages (reassembly).
   Everything between two NIs — routers, links, credits — operates on flits
   without understanding the payload.

7. **Virtual networks separate traffic classes.**
   Each protocol assigns messages to vnets.
   Each vnet gets its own VC pool and buffer sizing.
   This prevents deadlock and head-of-line blocking between traffic classes.

8. **Only the TAIL flit delivers the message.**
   HEAD and BODY flits are consumed for credits and discarded.
   The TAIL flit's `MsgPtr` is enqueued into the destination protocol buffer.

---
