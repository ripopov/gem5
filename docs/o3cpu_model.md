# The gem5 O3CPU Model: A Comprehensive Reference

## Table of Contents

1. [Introduction and Architectural Overview](#1-introduction-and-architectural-overview)
2. [Pipeline Organization](#2-pipeline-organization)
3. [Dynamic Instructions: The Unit of Work](#3-dynamic-instructions-the-unit-of-work)
4. [Inter-Stage Communication](#4-inter-stage-communication)
5. [Branch Address Calculation (BAC)](#5-branch-address-calculation-bac)
6. [Fetch Target Queue (FTQ)](#6-fetch-target-queue-ftq)
7. [Fetch Stage](#7-fetch-stage)
8. [Decode Stage](#8-decode-stage)
9. [Rename Stage](#9-rename-stage)
10. [Register Renaming Infrastructure](#10-register-renaming-infrastructure)
11. [Issue/Execute/Writeback (IEW) Stage](#11-issueexecutewriteback-iew-stage)
12. [Instruction Queue (IQ)](#12-instruction-queue-iq)
13. [Load/Store Queue (LSQ)](#13-loadstore-queue-lsq)
14. [Functional Unit Pool](#14-functional-unit-pool)
15. [Reorder Buffer (ROB)](#15-reorder-buffer-rob)
16. [Commit Stage](#16-commit-stage)
17. [Squash Mechanism](#17-squash-mechanism)
18. [Memory Dependence Prediction](#18-memory-dependence-prediction)
19. [Simultaneous Multithreading (SMT)](#19-simultaneous-multithreading-smt)
20. [CPU Lifecycle and Drain Support](#20-cpu-lifecycle-and-drain-support)
21. [Statistics and Observability](#21-statistics-and-observability)
22. [Configuration and Parameterization](#22-configuration-and-parameterization)

---

## 1. Introduction and Architectural Overview

The O3CPU is gem5's cycle-accurate model of a superscalar, out-of-order processor. It
faithfully represents the major microarchitectural structures found in modern high-performance
cores: branch prediction, register renaming, out-of-order issue, speculative execution, and
in-order retirement. The model comprises approximately 28,505 lines of C++ across 50 source
files in `src/cpu/o3/`, complemented by eight Python SimObject definitions.

The O3CPU models a processor pipeline that fetches multiple instructions per cycle,
renames architectural registers to physical registers to eliminate false dependencies,
issues instructions to functional units as soon as their operands become available (regardless
of program order), and retires them strictly in program order through a reorder buffer.

### 1.1 Design Philosophy

The model balances simulation fidelity against simulation speed. Several key design choices
reflect this:

- **Pre-decoded instructions.** Instructions are decoded into `StaticInst` objects at fetch
  time, not in a separate hardware-like decode stage. The pipeline "decode" stage serves as
  a buffer and early branch-resolution check point.

- **TimeBuffer-based communication.** Inter-stage communication uses parameterized-delay
  ring buffers (`TimeBuffer`) rather than explicit flip-flop models, allowing pipeline depth
  to be configured without code changes.

- **Activity-driven scheduling.** The CPU tracks whether any pipeline stage has useful work.
  When the entire pipeline is idle, the CPU deschedules itself from the event queue, avoiding
  simulation cycles on an inactive core.

- **Modular front-end.** The branch predictor and fetch stage can operate in either a
  tightly-coupled mode (traditional) or a decoupled mode where the branch predictor runs ahead
  independently, populating a Fetch Target Queue.

### 1.2 Source Code Map

```
src/cpu/o3/
├── cpu.{hh,cc}            CPU top-level: tick loop, stage orchestration
├── bac.{hh,cc}            Branch Address Calculation stage
├── ftq.{hh,cc}            Fetch Target Queue
├── fetch.{hh,cc}          Fetch stage
├── decode.{hh,cc}         Decode stage
├── rename.{hh,cc}         Rename stage
├── iew.{hh,cc}            Issue/Execute/Writeback stage
├── commit.{hh,cc}         Commit stage
├── dyn_inst.{hh,cc}       Dynamic instruction representation
├── inst_queue.{hh,cc}     Instruction queue / scheduler
├── lsq.{hh,cc}            Load/Store queue (top-level)
├── lsq_unit.{hh,cc}       Load/Store queue (per-thread unit)
├── rob.{hh,cc}            Reorder buffer
├── rename_map.{hh,cc}     Rename map tables
├── free_list.{hh,cc}      Physical register free lists
├── scoreboard.{hh,cc}     Register readiness scoreboard
├── dep_graph.hh           Dependency graph for IQ
├── mem_dep_unit.{hh,cc}   Memory dependence unit
├── store_set.{hh,cc}      Store set predictor
├── fu_pool.{hh,cc}        Functional unit pool
├── regfile.{hh,cc}        Physical register file
├── comm.hh                Inter-stage communication structures
├── limits.hh              Compile-time constants
├── thread_context.{hh,cc} Thread context wrapper
├── thread_state.{hh,cc}   Per-thread state
├── checker.{hh,cc}        Optional execution checker
├── dyn_inst_ptr.hh        Smart pointer typedef
└── probe/                 Probe points for external observers
```

---

## 2. Pipeline Organization

### 2.1 Pipeline Stages

The O3CPU pipeline consists of six logical stages, each modeled as a C++ class
instantiated as a direct member of the `CPU` object:

```
┌─────┐   ┌───────┐   ┌────────┐   ┌────────┐   ┌─────────────────────┐   ┌────────┐
│ BAC │──▶│ Fetch  │──▶│ Decode │──▶│ Rename │──▶│ IEW                 │──▶│ Commit │
│     │   │        │   │        │   │        │   │ (Issue+Exec+WB)     │   │        │
└─────┘   └────────┘   └────────┘   └────────┘   └─────────────────────┘   └────────┘
  │            ▲                                          │                     │
  │            │           Fetch Target Queue             │                     │
  └────────────┼──────────────────────────────────────────┘                     │
               │                                                               │
               └───────────────────────────────────────────────────────────────┘
                               Backwards communication (squash, stall, free entries)
```

### 2.2 Execution Order Within a Cycle

Each simulated cycle, the CPU's `tick()` method
([cpu.cc](src/cpu/o3/cpu.cc)) calls stage ticks in strict order:

```
1. BAC.tick()       Branch prediction / fetch target generation
2. Fetch.tick()     Instruction fetch from I-cache
3. Decode.tick()    Pipeline buffering and early branch check
4. Rename.tick()    Register renaming and resource allocation
5. IEW.tick()       Dispatch, execute, and writeback
6. Commit.tick()    In-order retirement
```

After all stages tick, the CPU advances all inter-stage time buffers
([cpu.cc](src/cpu/o3/cpu.cc)), making data written this cycle
visible to downstream consumers after the configured latency.

### 2.3 Pipeline Width

The pipeline width is configurable but bounded by a compile-time constant:

```
MaxWidth   = 16  (maximum instructions per cycle in any stage)
MaxThreads = 4   (maximum simultaneous hardware threads)
```

These constants are defined in [limits.hh](src/cpu/o3/limits.hh) and control the
static sizing of arrays in communication structures.

### 2.4 Conceptual Pipeline Timing

A typical instruction flows through the pipeline as follows. The latencies between
stages are configurable parameters:

```
Cycle  0:  (Decoupled mode) BAC may generate fetch target(s)
Cycle  1:  Fetch sends I-cache request, ITLB translation
Cycle  2:  I-cache returns data, instructions pre-decoded
Cycle  3:  Fetch queue → Decode receives instructions
Cycle  4:  Decode → Rename receives instructions
Cycle  5:  Rename → IEW dispatches to IQ/LSQ
Cycle  6+: IEW issues when operands ready, executes on FU
Cycle  N:  Execution completes, writeback wakes dependents
Cycle  N+1: Commit retires instruction from ROB head
```

In coupled front-end mode, branch prediction advances through `Fetch::buildInst()`
calling `BAC::updatePC()`; BAC does not independently run ahead.

---

## 3. Dynamic Instructions: The Unit of Work

Every instruction flowing through the O3 pipeline is represented by a `DynInst` object
([dyn_inst.hh](src/cpu/o3/dyn_inst.hh), [dyn_inst.cc](src/cpu/o3/dyn_inst.cc)),
which is the central data structure of the model. A `DynInst` wraps a `StaticInst` (the
decoded, ISA-level instruction) with all the dynamic, per-instance state needed to track
it through the pipeline.

### 3.1 Instruction Identity

Each dynamic instruction carries:

- A **sequence number** (`seqNum`): a globally unique, monotonically increasing identifier
  assigned at fetch time. Sequence numbers establish program order throughout the pipeline.
  All squash, commit, and dependency operations rely on comparing sequence numbers.

- A **static instruction** pointer (`staticInst`): the decoded, immutable representation of
  the instruction's operation. Multiple `DynInst` objects can share the same `StaticInst`
  (e.g., in a loop).

- A **thread identifier** (`threadNumber`): which hardware thread owns this instruction.

### 3.2 Lifecycle Status Tracking

A `DynInst` tracks its progress through the pipeline using a 24-bit status bitset. Multiple
bits can be set simultaneously. The key lifecycle states are:

```
Created ──▶ CanIssue ──▶ Issued ──▶ Executed ──▶ CanCommit ──▶ Committed
                                                      │
                                               Squashed (any point)
```

Additional status bits track queue membership (`IqEntry`, `RobEntry`, `LsqEntry`),
serialization requirements (`SerializeBefore`, `SerializeAfter`), and pinned register
management for partial writes.

### 3.3 Register Operand Storage

To minimize heap allocation overhead, `DynInst` uses a custom `operator new`
([dyn_inst.cc](src/cpu/o3/dyn_inst.cc)) that allocates a single
contiguous buffer containing the `DynInst` object itself plus trailing arrays for:

- Flattened architectural destination register IDs
- Physical destination register IDs (after rename)
- Previous physical register mappings (for squash rollback)
- Physical source register IDs (after rename)
- Per-source-register readiness flags

This single-allocation strategy improves cache locality and reduces allocator pressure.

### 3.4 Source Register Readiness

The field `readyRegs` counts how many source registers have been marked ready. When a
source register's producing instruction completes writeback, the IQ calls
`markSrcRegReady()` ([dyn_inst.cc](src/cpu/o3/dyn_inst.cc)),
which increments `readyRegs`. When `readyRegs` equals the total number of source registers,
the instruction is marked `CanIssue` and becomes eligible for scheduling.

### 3.5 Branch Prediction Data

Each `DynInst` stores the predicted next PC (`predPC`). The `mispredicted()` method
([dyn_inst.hh](src/cpu/o3/dyn_inst.hh)) computes the actual next PC
by calling `staticInst->advancePC()` and compares it against `predPC`. If they differ,
the branch predictor was wrong, and a squash is initiated.

### 3.6 Pipeline Tick Stamps

Each instruction records timestamps as it passes through pipeline stages:

```
fetchTick → decodeTick → renameTick → dispatchTick → issueTick → completeTick → commitTick
```

These are used by the `O3PipeView` tracing facility to generate pipeline visualizations.

### 3.7 Memory Data

For load and store instructions, the `DynInst` carries the effective address, physical
address, memory request size, request flags, a data buffer pointer (`memData`), and
iterators into the load queue and store queue.

### 3.8 Deferred Miscellaneous Register Writes

Instructions that write to miscellaneous (control/status) registers are recorded for
commit-time application via `updateMiscRegs()`
([dyn_inst.hh](src/cpu/o3/dyn_inst.hh)).
For non-serializing misc registers on non-speculative instructions, the model may also
apply the write immediately in `setMiscRegOperand()` while still keeping the deferred
commit-time record.

---

## 4. Inter-Stage Communication

### 4.1 The TimeBuffer Abstraction

Stages communicate through `TimeBuffer` objects -- parameterized ring buffers that model
pipeline register delays. A `TimeBuffer<T>` provides:

- A **write wire** at offset 0 (current cycle).
- **Read wires** at negative offsets representing past cycles.

When a stage writes data to its output wire, that data becomes readable by the downstream
stage after the configured number of cycles, naturally modeling pipeline latency without
explicit delay counters.

### 4.2 Forward Data Flow

Instructions flow forward through dedicated `TimeBuffer` queues, each carrying a
fixed-size array of instruction pointers:

```
┌───────────┐   FetchStruct    ┌──────────┐   DecodeStruct   ┌──────────┐
│   Fetch   │ ─────────────▶  │  Decode   │ ──────────────▶ │  Rename  │
└───────────┘  (fetchQueue)    └──────────┘  (decodeQueue)   └──────────┘
                                                                  │
                                                           RenameStruct
                                                          (renameQueue)
                                                                  │
                                                                  ▼
┌──────────┐   IEWStruct       ┌──────────┐
│  Commit  │ ◀──────────────  │   IEW    │
└──────────┘  (iewQueue)       └──────────┘
```

Each structure carries up to `MaxWidth` (16) instruction pointers and a `size` count.

### 4.3 Backward Data Flow

All backward communication (squash signals, stall signals, resource availability counts)
flows through a single shared `TimeBuffer<TimeStruct>`
([comm.hh](src/cpu/o3/comm.hh)). This structure contains per-stage
sub-structures:

```
TimeStruct
├── FetchComm[MaxThreads]    Fetch → BAC: block, squash, redirect PC
├── DecodeComm[MaxThreads]   Decode → Fetch/BAC: misprediction, squash
├── IewComm[MaxThreads]      IEW → Rename: free IQ/LQ/SQ entries, dispatch counts
├── CommitComm[MaxThreads]   Commit → all: squash, ROB status, trap/interrupt
├── decodeBlock/Unblock      Decode → Fetch: flow control
├── renameBlock/Unblock      Rename → Decode: flow control
└── iewBlock/Unblock         IEW → Rename: flow control
```

Each stage reads from this buffer at a negative offset corresponding to the pipeline
distance. For example, Fetch reads Commit's signals at offset `-commitToFetchDelay`.

### 4.4 Block/Unblock Protocol

When a downstream stage cannot accept more instructions (e.g., the IQ is full), it sends
a `block` signal to the upstream stage. The upstream stage responds by:

1. Saving unprocessed instructions into a **skid buffer** (a per-thread FIFO).
2. Ceasing to process new instructions until an `unblock` signal arrives.
3. When unblocked, draining the skid buffer before resuming normal operation.

The skid buffer is sized to absorb all instructions that could be in-flight between the
two stages: `skidBufferMax = (inter_stage_delay + 1) × stage_width`.

### 4.5 Internal IEW Communication

Within the IEW stage, the instruction queue schedules instructions for execution through
an internal `TimeBuffer<IssueStruct>` with configurable delay (`issueToExecuteDelay`).
This models the time between instruction selection and functional unit dispatch.

---

## 5. Branch Address Calculation (BAC)

The BAC stage ([bac.hh](src/cpu/o3/bac.hh), [bac.cc](src/cpu/o3/bac.cc)) is the
interface between the branch predictor and the rest of the pipeline. It operates in one
of two modes depending on the `decoupledFrontEnd` configuration parameter.

### 5.1 Coupled Mode (Default)

In coupled mode, the BAC is passive. It does not generate fetch targets independently.
Instead, the fetch stage calls `BAC::updatePC()` after pre-decoding each instruction.
For branch instructions, `updatePC()` queries the branch predictor directly:

```
Fetch pre-decodes instruction
    │
    ▼
BAC::updatePC(instruction, next_pc, fetch_target)
    │
    ├── Is this a branch? ──▶ BPU::predict() ──▶ Update next_pc with predicted target
    │
    └── Not a branch ──▶ Advance PC normally (staticInst->advancePC)
```

Branch predictor updates on commit and squash corrections also flow through the BAC,
which calls `BPU::update()` and `BPU::squash()` respectively.

### 5.2 Decoupled Mode

In decoupled mode, the BAC runs ahead of fetch, acting as an independent prediction
engine that populates the Fetch Target Queue (FTQ). Each cycle, `generateFetchTargets()`
([bac.cc](src/cpu/o3/bac.cc)) performs the following:

```
┌─────────────────────────────────────────────────────────────────────┐
│ BAC::generateFetchTargets() -- one cycle                            │
│                                                                     │
│  1. Start at current BAC PC                                         │
│  2. Scan consecutive addresses using BTB lookup                     │
│     (increment by minInstSize: 1 byte for x86, 4 bytes for ARM)     │
│  3. When branch found in BTB:                                       │
│     a. Query branch predictor for direction                         │
│     b. Create FetchTarget with [startPC, endPC, predPC, taken]      │
│     c. Attach BPU history to the FetchTarget                        │
│     d. Insert FetchTarget into FTQ                                  │
│  4. Update BAC PC for next iteration                                │
│  5. Repeat until bandwidth limit or FTQ full                        │
│                                                                     │
│  Bandwidth limits:                                                  │
│    maxFTPerCycle        -- max fetch targets generated per cycle    │
│    maxTakenPredPerCycle -- max taken branches predicted per cycle   │
└─────────────────────────────────────────────────────────────────────┘
```

### 5.3 Pre-Decode Reconciliation

In decoupled mode, when Fetch actually pre-decodes an instruction and discovers it is
a branch, it calls `BAC::updatePreDecode()`
([bac.cc](src/cpu/o3/bac.cc)) to reconcile the fetch-time decode
with the earlier BAC prediction. This handles cases such as:

- The BTB correctly predicted the branch location (most common case -- simply transfers
  the BPU history from the FetchTarget to the instruction).
- The BTB missed the branch entirely (creates a "dummy" not-taken history).
- The branch type was misidentified (e.g., BTB recorded it as conditional but it is
  actually unconditional).

### 5.4 Squash Handling

When a squash occurs (from commit, decode, or fetch), the BAC must undo speculative
branch predictor state. The method `squashBpuHistories()`
([bac.cc](src/cpu/o3/bac.cc)) iterates the FTQ backwards and calls
`BPU::squashHistory()` for each FetchTarget that has an attached predictor history,
reverting the predictor to its pre-speculation state.

### 5.5 Per-Thread Status Model

```
┌──────┐     instructions     ┌─────────┐
│ Idle │ ──────────────────▶ │ Running │
└──────┘                      └────┬────┘
                                   │
                 ┌─────────────────┼──────────────────────────┐
                 ▼                 ▼                          ▼
           ┌──────────┐      ┌─────────┐                ┌──────────┐
           │ FTQFull  │      │Squashing│                │ Blocked  │
           └──────────┘      └─────────┘                └──────────┘
                 │
                 ▼
           ┌──────────┐
           │FTQLocked │
           └──────────┘
```

---

## 6. Fetch Target Queue (FTQ)

The FTQ ([ftq.hh](src/cpu/o3/ftq.hh), [ftq.cc](src/cpu/o3/ftq.cc)) is the bridge
between the BAC and the Fetch stage in decoupled front-end mode. It is a per-thread
FIFO of `FetchTarget` objects.

### 6.1 FetchTarget Structure

Each FetchTarget represents a contiguous sequence of instructions ending at a branch
(or the maximum fetch target width):

```
┌─────────────────────────────────────────────────────────────┐
│ FetchTarget                                                 │
│                                                             │
│  startPC ─────────────────────────────── endPC              │
│  │                                        │                 │
│  ▼                                        ▼                 │
│  ┌────┬────┬────┬────┬────┬────┬────┬────┐                  │
│  │inst│inst│inst│inst│inst│inst│inst│ BR │                  │
│  └────┴────┴────┴────┴────┴────┴────┴──┬─┘                  │
│                                        │                    │
│  is_branch: true                       ▼                    │
│  taken: true/false              predPC (predicted target)   │
│  bpuHistory: attached predictor state                       │
│  ftSeqNum: ordering number                                  │
└─────────────────────────────────────────────────────────────┘
```

### 6.2 FTQ Operations

The FTQ provides these operations:

- **insert()**: BAC pushes a new FetchTarget to the tail.
- **readHead()**: Fetch peeks at the head FetchTarget without removing it.
- **popHead()**: Fetch removes the head FetchTarget after consuming all its instructions.
  Returns false if the head still has attached BPU history, or if the FTQ is `Locked`
  (in which case status transitions to `Invalid` and a squash is required to recover).
- **squash()**: Clears all entries on a squash, resetting to `Valid` state.

### 6.3 FTQ Status Model

The `FTQ::Status` enum includes `Invalid`, `Valid`, `Full`, and `Locked`. In the
current implementation, control flow uses `Invalid`, `Valid`, and `Locked`; queue
fullness is determined by occupancy (`isFull()`), not by setting `ftqStatus = Full`.

```
┌─────────┐   squash/reset   ┌───────┐
│ Invalid │ ───────────────▶ │ Valid │
└────┬────┘                   └───┬───┘
     │                            │ lock()
     │ popHead fail / locked pop  ▼
     └────────────────────────┌────────┐
                              │ Locked │
                              └────────┘

Occupancy (`size >= numEntries`) determines "fullness" via `isFull()`.
```

### 6.4 Coupled vs. Decoupled Behavior

In coupled mode, the FTQ is still instantiated but plays a minimal role. `readHead()`
returns `nullptr` when the queue is empty, as usual. The key behavioral difference is that
`Fetch::ftqReady()` always returns true when `decoupledFrontEnd` is disabled, so fetch
does not stall on FTQ head readiness.

---

## 7. Fetch Stage

The Fetch stage ([fetch.hh](src/cpu/o3/fetch.hh), [fetch.cc](src/cpu/o3/fetch.cc))
is one of the largest stages and handles instruction
retrieval from the instruction cache, pre-decoding, macro-op expansion, and branch
prediction coordination.

### 7.1 High-Level Architecture

```
                              ┌──────────────────────────────┐
                              │          Fetch Stage         │
                              │                              │
I-Cache ◀──────────────▶      │  ┌─────────┐  ┌──────────┐  │
(IcachePort)                  │  │  Fetch  │  │  Fetch   │  │
                              │  │ Buffer  │─▶│  Queue   │──┼──▶ To Decode
ITLB ◀──────────────────▶     │  │ (bytes) │  │ (insts)  │  │
(FetchTranslation)            │  └─────────┘  └──────────┘  │
BAC/FTQ ◀──────────────▶      │        ▲                     │
                              │        │ ISA Decoder         │
                              └────────┼─────────────────────┘
                                       │
                                 fetchCacheLine()
                                 finishTranslation()
                                 processCacheCompletion()
```

### 7.2 Per-Thread State

Fetch maintains independent state for each hardware thread:

- **PC and fetch offset**: Current program counter and byte offset within a macro-op.
- **Fetch buffer**: A byte buffer (size configurable, up to one cache line) holding
  raw instruction bytes from the I-cache.
- **Fetch queue**: A `std::deque<DynInstPtr>` holding pre-decoded instructions waiting
  to be drained to decode. Maximum size is configurable (`fetchQueueSize`).
- **ISA decoder**: Per-thread decoder instance for converting raw bytes to `StaticInst`.
- **Thread status**: A 13-state machine tracking what the thread is doing.

### 7.3 Thread Status Model

```
                    ┌──────────────────────────────────────────────┐
                    │              Fetch Thread States             │
                    │                                              │
                    │  Running  ◀────────▶  Blocked                │
                    │     │                     ▲                  │
                    │     │                     │ stall            │
                    │     ▼                     │                  │
                    │  Fetching ─▶ ItlbWait ─▶ IcacheWaitResponse  │
                    │                     │      │                 │
                    │                     ▼      │                 │
                    │          IcacheAccessComplete                │
                    │                     │                        │
                    │                     ▼                        │
                    │                  Running                     │
                    │                                              │
                    │  Special: Squashing, Idle, TrapPending,      │
                    │           QuiescePending, FtqWait,           │
                    │           IcacheWaitRetry, NoGoodAddr        │
                    └──────────────────────────────────────────────┘
```

### 7.4 The Fetch Algorithm

The `tick()` method ([fetch.cc](src/cpu/o3/fetch.cc)) orchestrates
each cycle:

**Step 1: Check backward signals.**
For every active thread, `checkSignalsAndUpdate()` processes squash signals from commit
and decode, stall signals, and transitions the thread's state machine. Commit squashes
take priority over decode squashes.

**Step 2: Handle interrupts (FullSystem mode).**
Reads interrupt pending/clear signals from commit and updates the internal
`interruptPending` flag.

**Step 3: Fetch instructions.**
Calls `fetch()` up to `numFetchingThreads` times, each call servicing one thread.

**Step 4: Drain fetch queue to decode.**
Moves instructions from per-thread fetch queues to the decode output wire, up to
`decodeWidth` instructions per cycle. Uses random round-robin across threads for fair
SMT bandwidth sharing.

### 7.5 The Core Fetch Loop

The `fetch()` method ([fetch.cc](src/cpu/o3/fetch.cc)) is the
heart of the stage. For the selected thread:

**Phase 1: Thread selection.** The SMT fetch policy selects which thread to service.

**Phase 2: FTQ readiness.** In decoupled mode, checks if the FTQ has a ready fetch target.

**Phase 3: I-cache state machine.** Determines whether the fetch buffer contains valid
data for the current PC. If not, initiates a new I-cache access:

```
fetchCacheLine(vaddr) ──▶ ITLB translation ──▶ finishTranslation()
                                                      │
                                        ┌─────────────┼──────────────┐
                                        ▼             ▼              ▼
                                    No fault       TLB fault    No system memory
                                        │              │              │
                                        ▼              ▼              ▼
                              sendTimingReq()    Build NOP    NoGoodAddr status
                                   to I-cache    carrying fault
                                        │
                           ┌────────────┼────────────┐
                           ▼                         ▼
                       Success                   Failure
                   IcacheWaitResponse         IcacheWaitRetry
                           │                    (retry later)
                           ▼
                processCacheCompletion()
                   (copy data to fetchBuffer)
```

**Phase 4: Instruction decode loop.** While bandwidth and queue capacity permit:

1. Feed raw bytes from the fetch buffer into the ISA decoder.
2. Extract decoded `StaticInst` objects. If a macro-op, iterate its micro-ops.
3. For each instruction, call `buildInst()` to create a `DynInst` with a unique
   sequence number.
4. Call `BAC::updatePC()` for branch prediction. If a taken branch is predicted,
   stop fetching (the next fetch target starts at the predicted target).
5. Push the instruction onto the per-thread fetch queue.

**Phase 5: Pipelined I-cache prefetch.** If the next instruction address crosses a
fetch buffer boundary, set a flag to issue the next I-cache access at the end of the
cycle, hiding the cache latency.

### 7.6 I-Cache Interface

The fetch stage communicates with the instruction cache through an `IcachePort`
(a `RequestPort` subclass). The protocol follows gem5's timing memory model:

- **Request**: `sendTimingReq(PacketPtr)` sends a read request.
- **Response**: `recvTimingResp(PacketPtr)` is called when data arrives.
- **Retry**: If the cache rejects a request (MSHR full), the packet is saved.
  When `recvReqRetry()` is called, the saved packet is retransmitted.

### 7.7 Squash Handling

The `doSquash()` method ([fetch.cc](src/cpu/o3/fetch.cc)):

1. Sets the PC to the squash target address.
2. Resets the fetch offset and macro-op state.
3. Resets the ISA decoder.
4. If waiting on an I-cache response or ITLB translation, invalidates the pending
   request (the response will be silently dropped when it arrives).
5. Clears the fetch queue for the squashed thread.

### 7.8 SMT Thread Selection Policies

```
┌────────────────────────────────────────────────────────┐
│ Policy          │ Algorithm                            │
├────────────────────────────────────────────────────────┤
│ RoundRobin      │ Rotate through threads in order.     │
│                 │ Move serviced thread to back of list.│
├────────────────────────────────────────────────────────┤
│ IQCount         │ Favor thread with fewest IQ entries. │
│                 │ Reduces pressure on the busiest      │
│                 │ thread's instruction queue.          │
├────────────────────────────────────────────────────────┤
│ LSQCount        │ Favor thread with fewest LSQ entries.│
│                 │ Prevents one thread from monopolizing│
│                 │ the load/store queue.                │
├────────────────────────────────────────────────────────┤
│ Branch          │ Not yet implemented.                 │
└────────────────────────────────────────────────────────┘
```

---

## 8. Decode Stage

The Decode stage ([decode.hh](src/cpu/o3/decode.hh), [decode.cc](src/cpu/o3/decode.cc))
is perhaps the most misnamed stage in the pipeline. Instructions arriving here are
**already decoded** -- the `StaticInst` was created during fetch. The Decode stage
serves three purposes:

1. **Pipeline buffer** between fetch and rename.
2. **Early branch resolution** for direct (PC-relative) branches.
3. **Flow control** via blocking, unblocking, and squash propagation.

### 8.1 Early Branch Resolution

The most important functional contribution of the decode stage is checking direct
branch targets ([decode.cc](src/cpu/o3/decode.cc)):

```
For each instruction passing through decode:
    If instruction is a DIRECT control flow instruction
    AND (unconditional OR predicted taken):
        1. Compute actual target from instruction encoding
        2. Compare with predicted target from branch predictor
        3. If mismatch:
           a. Squash all younger instructions
           b. Redirect fetch to correct target
           c. Record as branch misprediction
```

This catches mispredictions for PC-relative branches (e.g., direct jumps, conditional
branches with known offsets) several cycles earlier than waiting for the execute stage.
Indirect branches (register-based targets) can only be resolved at execute.

### 8.2 Decode Processing Loop

The `decodeInsts()` method ([decode.cc](src/cpu/o3/decode.cc))
processes instructions from either the input queue or the skid buffer:

```
For each instruction (up to decodeWidth per cycle, shared across all threads):
    1. Skip if already squashed
    2. If zero source registers → mark as immediately issuable (optimization)
    3. Forward to rename via DecodeStruct output
    4. Record decode latency on the instruction (decodeTick)
    5. Check for direct branch target mismatch → squash if wrong
```

An important detail: `toRenameIndex` is shared across all threads within a single cycle,
meaning the decode bandwidth is divided among active SMT threads, not replicated.

### 8.3 Per-Thread Status Machine

```
                    ┌────────┐
                    │  Idle  │
                    └───┬────┘
                        │ instructions arrive
                        ▼
    squash ──▶ ┌─────────────┐ ◀── stall cleared
               │   Running   │
               └──────┬──────┘
                      │ rename stalls
                      ▼
               ┌──────────────┐
               │   Blocked    │──▶ skid buffer absorbs instructions
               └──────┬───────┘
                      │ stall cleared
                      ▼
               ┌──────────────┐
               │  Unblocking  │──▶ drain skid buffer, then → Running
               └──────────────┘
```

### 8.4 Stall Source

The decode stage has exactly **one** stall source: the rename stage. When rename is
full (ROB full, IQ full, LSQ full, or free register list empty), it signals decode to
block. Decode then pushes unprocessed instructions into its skid buffer and signals
fetch to stall.

---

## 9. Rename Stage

The Rename stage ([rename.hh](src/cpu/o3/rename.hh), [rename.cc](src/cpu/o3/rename.cc))
performs register renaming -- the key transformation that enables out-of-order execution.
By mapping architectural (logical) register names to physical register names, it
eliminates false dependencies (WAR and WAW hazards) while preserving true dependencies
(RAW hazards).

### 9.1 The Rename Algorithm

For each instruction, `renameInsts()`
([rename.cc](src/cpu/o3/rename.cc)) performs:

```
For each instruction (up to renameWidth per cycle):

    1. CHECK DOWNSTREAM RESOURCES
       ├── ROB entries available?       ──▶ Block if not (ROBFullEvent)
       ├── IQ entries available?        ──▶ Block if not (IQFullEvent)
       ├── LQ entries available? (load) ──▶ Block if not (LQFullEvent)
       ├── SQ entries available? (store)──▶ Block if not (SQFullEvent)
       └── Physical registers free?     ──▶ Block if not (fullRegistersEvent)

    2. HANDLE SERIALIZATION
       ├── SerializeBefore? ──▶ Stall until ROB is empty
       └── SerializeAfter?  ──▶ Mark NEXT instruction as SerializeBefore

    3. RENAME SOURCE REGISTERS
       For each source register:
         a. Flatten architectural register via ISA
         b. Look up physical register in rename map
         c. Check scoreboard for readiness
         d. Update instruction with physical register ID

    4. RENAME DESTINATION REGISTERS
       For each destination register:
         a. Flatten architectural register via ISA
         b. Allocate new physical register from free list
         c. Update rename map: arch reg → new phys reg
         d. Record old mapping in history buffer for rollback
         e. Mark new register as NOT ready in scoreboard

    5. FORWARD TO IEW
       Place renamed instruction in RenameStruct for IEW
```

### 9.2 The Rename History Buffer

The history buffer is a per-thread list of `RenameHistory` entries, with the most
recent rename at the front:

```
┌──────────────────────────────────────────────────────────┐
│ RenameHistory Entry                                      │
│                                                          │
│  instSeqNum:  Instruction that caused this rename        │
│  archReg:     Flattened architectural register ID        │
│  newPhysReg:  Newly allocated physical register          │
│  prevPhysReg: Physical register previously mapped        │
└──────────────────────────────────────────────────────────┘
```

The history buffer serves two purposes:

**On commit** (`removeFromHistory()`,
[rename.cc](src/cpu/o3/rename.cc)): The **old** physical register
(`prevPhysReg`) is returned to the free list. The new mapping is now architecturally
permanent.

**On squash** (`doSquash()`,
[rename.cc](src/cpu/o3/rename.cc)): The **new** physical register
(`newPhysReg`) is returned (via deferred freeing), and the rename map is restored to
point at `prevPhysReg`.

```
                     ┌─── Committed ───┐
                     │                  │
  Instruction ──▶ History ──▶ Free prevPhysReg (old mapping superseded)
                     │
                     │                  │
                     └─── Squashed ────┘
                                        │
                               Restore prevPhysReg mapping
                               Defer free of newPhysReg
```

### 9.3 Deferred Register Freeing for SMT Safety

When a squash occurs, the newly allocated physical registers cannot be immediately
returned to the free list. In an SMT system, another thread's in-flight instructions
might still be reading these registers (because instructions from different threads
share the physical register file).

Instead, squashed registers are placed in `freeingInProgress[tid]`
([rename.cc](src/cpu/o3/rename.cc)). They are actually freed only after
commit confirms that the ROB has finished squashing
([rename.cc](src/cpu/o3/rename.cc)).

### 9.4 Serialization Protocol

Some instructions require the pipeline to be empty before they execute (e.g., system
register writes, store conditionals). The rename stage enforces this through two
mechanisms:

- **SerializeBefore**: The instruction is held in `serializeInst[tid]` and the stage
  enters `SerializeStall`. It waits until `emptyROB[tid]` is true and
  `instsInProgress[tid]` is zero. Then the instruction is re-inserted at the front
  of the queue with its serialize flag cleared.

- **SerializeAfter**: The instruction itself proceeds normally, but the next instruction
  in the queue is automatically marked `SerializeBefore` via `serializeAfter()`.

### 9.5 Resource Tracking

Rename maintains cached counts of free downstream entries and adjusts them based on
in-flight instructions:

```
Effective Free Entries = Cached Free Entries - (Sent to IEW - Dispatched by IEW)
```

This allows rename to make conservative flow-control decisions without waiting for
round-trip confirmation from downstream stages.

### 9.6 Stall Conditions Summary

```
┌────────────────────────────────────────────────────────────┐
│ Condition              │ Source              │ Resolution  │
├────────────────────────────────────────────────────────────┤
│ ROB full               │ Commit reporting    │ Commit frees│
│ IQ full                │ IEW reporting       │ IEW frees   │
│ LQ full                │ IEW reporting       │ Load commits│
│ SQ full               │ IEW reporting       │ Store commits│
│ No free phys registers │ Free list empty     │ Commit frees│
│ IEW stall signal       │ IEW blocking        │ IEW unblocks│
│ Serialize stall        │ Instruction flag    │ ROB drains  │
└────────────────────────────────────────────────────────────┘
```

---

## 10. Register Renaming Infrastructure

### 10.1 The Rename Map

The renaming infrastructure consists of two layers:

**`SimpleRenameMap`** ([rename_map.hh](src/cpu/o3/rename_map.hh)):
A per-register-class mapping table. Internally, it is a vector indexed by architectural
register number, storing the current physical register pointer.

**`UnifiedRenameMap`** ([rename_map.hh](src/cpu/o3/rename_map.hh)):
Wraps one `SimpleRenameMap` per register class (integer, floating-point, vector, vector
element, predicate, matrix, condition code). It dispatches rename/lookup operations to
the appropriate per-class map.

### 10.2 The Rename Operation

When `SimpleRenameMap::rename()` is called:

```
1. Save current mapping: prev_reg = map[arch_reg]
2. If the register class is InvalidRegClass:
     Return (prev_reg, prev_reg) -- no rename needed
3. If the register is "pinned" (pinned write count > 0):
     Decrement pin count, return (prev_reg, prev_reg) -- same register
4. Otherwise (normal rename):
     new_reg = freeList->getReg()         -- pop from free list
     map[arch_reg] = new_reg              -- update mapping
     Return (new_reg, prev_reg)           -- for history buffer
```

Miscellaneous registers (e.g., control/status registers) are not renamed -- they have
fixed physical register mappings obtained from the register file.

### 10.3 The Free List

**`SimpleFreeList`** ([free_list.hh](src/cpu/o3/free_list.hh)):
A FIFO queue of physical register pointers for a single register class. `getReg()` pops
from the front (allocation), `addReg()` pushes to the back (deallocation).

**`UnifiedFreeList`** ([free_list.hh](src/cpu/o3/free_list.hh)):
Wraps one `SimpleFreeList` per register class. Initialized by the physical register file,
which populates each free list with all available physical registers at startup.

### 10.4 The Scoreboard

The scoreboard ([scoreboard.hh](src/cpu/o3/scoreboard.hh)) is a flat boolean vector
indexed by physical register flat index:

```
┌───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┬───┐
│ 1 │ 1 │ 0 │ 1 │ 1 │ 0 │ 1 │ 1 │ 1 │ 0 │ 1 │...│
└───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
  p0  p1  p2  p3  p4  p5  p6  p7  p8  p9  p10

  1 = register value is ready (produced by a completed instruction)
  0 = register value is not yet ready (awaiting execution)
```

- **Set** (`setReg`): Called during writeback when an instruction's result is available.
- **Unset** (`unsetReg`): Called during rename when a register is assigned as a new
  destination.
- **Query** (`getReg`): Called during rename to determine if a source register is ready.

### 10.5 Dual Rename Maps

The CPU maintains **two** rename maps per thread:

- **Speculative rename map** (`renameMap[tid]`): Used by the rename stage. Updated
  speculatively as instructions are renamed. Rolled back on squash.
- **Committed rename map** (`commitRenameMap[tid]`): Updated at commit time. Represents
  the architecturally correct register mapping. Used for external register access (e.g.,
  debugger reads).

---

## 11. Issue/Execute/Writeback (IEW) Stage

The IEW stage ([iew.hh](src/cpu/o3/iew.hh), [iew.cc](src/cpu/o3/iew.cc)) is the
widest and most complex stage in the pipeline. It combines three sub-functions that
would be separate stages in a deeper pipeline:

```
┌─────────────────────────────────────────────────────────────────────┐
│                        IEW Stage                                    │
│                                                                     │
│  ┌──────────┐    ┌─────────────────┐    ┌───────────────────────┐   │
│  │ Dispatch │───▶│  Issue (IQ)     │───▶│ Execute (FU + LSQ)    │   │
│  │          │    │  (schedule when │    │ (run on functional    │   │
│  │ (insert  │    │   operands      │    │  units or send to     │   │
│  │  into    │    │   ready)        │    │  memory)              │   │
│  │  IQ/LSQ) │    │                 │    │                       │   │
│  └──────────┘    └─────────────────┘    └───────────┬───────────┘   │
│                                                      │              │
│                                              ┌───────▼───────┐      │
│                                              │   Writeback   │      │
│                                              │ (wake deps,   │      │
│                                              │  update       │      │
│                                              │  scoreboard)  │      │
│                                              └───────────────┘      │
└─────────────────────────────────────────────────────────────────────┘
                                │
                                └──────────────────────────────▶ To Commit
```

### 11.1 Dispatch

The `dispatchInsts()` method
([iew.cc](src/cpu/o3/iew.cc)) takes renamed instructions from
the rename queue (or skid buffer) and inserts them into the appropriate structures:

```
For each instruction (up to dispatchWidth per cycle):

    If squashed: skip, return credits to rename
    If IQ full: block, record IQFullEvent
    If LQ full (load): block, record LSQFullEvent
    If SQ full (store): block, record LSQFullEvent

    Classification and insertion:
    ┌──────────────┬──────────────────────────────────────────┐
    │ Type         │ Action                                   │
    ├──────────────┼──────────────────────────────────────────┤
    │ Load         │ Insert into LQ + insert into IQ (normal) │
    │ Store        │ Insert into SQ + insert into IQ (normal) │
    │ Store Cond. │ Insert into SQ + insert as non-speculative│
    │ Atomic      │ Insert into SQ + insert as non-speculative│
    │ Barrier      │ Insert as barrier + non-spec entry in IQ │
    │ NOP          │ Immediately mark complete (no IQ/LSQ)    │
    │ Non-spec     │ Insert as non-speculative                │
    │ Regular      │ Insert into IQ (normal)                  │
    └──────────────┴──────────────────────────────────────────┘
```

### 11.2 Execute

The `executeInsts()` method
([iew.cc](src/cpu/o3/iew.cc)) runs instructions that were
scheduled by the IQ in a previous cycle:

```
For each instruction delivered via issueToExecQueue:

    If squashed: mark complete, skip

    If memory reference:
        ├── Load:   ldstQueue.executeLoad()
        ├── Store:  ldstQueue.executeStore()
        └── Atomic: ldstQueue.executeStore()

        If translation delayed (HW page walk): defer instruction
        If fault on store: mark executed, send to commit

    If non-memory:
        Execute via inst->execute()
        If predicate false: forward old register values
        Mark executed, send to commit

    After execution:
        Check for branch misprediction → squashDueToBranch()
        Check for memory ordering violation → squashDueToMemOrder()
```

### 11.3 Writeback

The `writebackInsts()` method
([iew.cc](src/cpu/o3/iew.cc)) processes completed instructions:

```
For each completed instruction (up to wbWidth per cycle):

    1. Fire ppToCommit probe point
    2. If not squashed AND executed AND no fault:
       a. instQueue.wakeDependents(inst) → wake all consumers
       b. For each destination register:
            scoreboard.setReg(dest) → mark register as ready
    3. Record writeback statistics
```

### 11.4 Writeback Bandwidth Management

When `instToCommit()` ([iew.cc](src/cpu/o3/iew.cc)) sends a
completed instruction to the commit queue, it manages bandwidth by tracking slot
occupancy:

```
If current cycle slot is full (wbNumInst == wbWidth):
    Advance to next cycle slot (++wbCycle, reset wbNumInst)
    Place instruction in the future cycle slot
```

This ensures that burst completions (e.g., multiple single-cycle ALU operations
finishing simultaneously) are spread across multiple cycles in the commit queue.

### 11.5 Squash Initiation from IEW

IEW can initiate squashes for two reasons:

**Branch misprediction** (`squashDueToBranch()`,
[iew.cc](src/cpu/o3/iew.cc)):
- The mispredicting instruction itself is NOT squashed (it executed correctly, just
  predicted the wrong direction).
- `includeSquashInst = false`.

**Memory ordering violation** (`squashDueToMemOrder()`,
[iew.cc](src/cpu/o3/iew.cc)):
- The violating instruction IS included in the squash (it must be re-executed).
- `includeSquashInst = true`.
- Memory violations take priority over branch mispredictions for the same instruction.

### 11.6 IEW tick() Execution Order

Within a single cycle, IEW operations execute in this order:

```
1. ldstQueue.tick()              Advance LSQ internal state
2. sortInsts()                   Distribute instructions from rename by thread
3. fuPools.processFreeUnits()    Free completed functional units
4. checkSignalsAndUpdate()       Process squash/stall signals from commit
5. dispatch()                    Insert instructions into IQ/LSQ
6. executeInsts()                Execute scheduled instructions
7. writebackInsts()              Wake dependents, update scoreboard
8. instQueue.scheduleReadyInsts() Schedule instructions for NEXT cycle
9. issueToExecQueue.advance()    Advance internal pipeline buffer
10. ldstQueue.writebackStores()  Send committed stores to memory
11. Process commit signals       Commit IQ/LSQ entries, schedule non-spec insts
12. Broadcast free entries       Send IQ/LQ/SQ counts to rename and fetch
```

---

## 12. Instruction Queue (IQ)

The Instruction Queue ([inst_queue.hh](src/cpu/o3/inst_queue.hh),
[inst_queue.cc](src/cpu/o3/inst_queue.cc)) is the scheduler at the heart of the
out-of-order engine. It tracks instruction dependencies, determines when instructions
are ready to execute, and selects which ready instructions to issue to functional units.

### 12.1 Architecture

The IQ has a two-level design:

- **`IQUnit`**: A SimObject representing one physical instruction queue partition with
  its own FU pool and SMT-aware entry tracking. Multiple IQUnits can be configured.
- **`InstructionQueue`**: The top-level coordinator that manages all IQUnits, the
  dependency graph, scheduling, and squashing.

### 12.2 Dependency Graph

The core data structure for tracking register dependencies is an array of singly-linked
lists ([dep_graph.hh](src/cpu/o3/dep_graph.hh)), sized to the number of physical
registers:

```
Dependency Graph (one list per physical register)

  PhysReg 0:  [Producer inst A] → [Consumer inst X] → [Consumer inst Y] → null
  PhysReg 1:  [Producer inst B] → [Consumer inst Z] → null
  PhysReg 2:  [null (no producer)]
  PhysReg 3:  [Producer inst C] → null
  ...
  PhysReg N:  [Producer inst D] → [Consumer inst W] → null
```

When an instruction is inserted into the IQ:

- **`addToProducers()`**: For each destination register, the instruction is set as the
  producer in the dependency graph head node. The register is marked not-ready in the
  IQ's internal scoreboard.

- **`addToDependents()`**: For each source register that is not yet ready, the instruction
  is linked into the dependency chain as a consumer.

When an instruction completes execution:

- **`wakeDependents()`**: For each destination register, all consumer instructions are
  popped from the chain and their source register readiness is updated. When all sources
  of a consumer are ready, it becomes eligible for scheduling.

### 12.3 The Scheduling Algorithm

The `scheduleReadyInsts()` method
([inst_queue.cc](src/cpu/o3/inst_queue.cc)) uses an
**oldest-first, cross-class** scheduling policy:

```
Ready Instruction Queues (one min-heap per OpClass, ordered by seqNum):

  IntAlu:    [inst seqNum=5] [inst seqNum=12] [inst seqNum=20]
  IntMult:   [inst seqNum=8] [inst seqNum=15]
  FpAlu:     [inst seqNum=3] [inst seqNum=10]
  MemRead:   [inst seqNum=7]
  MemWrite:  [inst seqNum=14]

Age Order List (sorted by oldest ready instruction across classes):
  FpAlu(3) → IntAlu(5) → MemRead(7) → IntMult(8) → FpAlu(10) → ...
```

The scheduler iterates through the age order list, issuing up to `issueWidth`
instructions per cycle:

```
For each entry in the age order list (oldest first):
    1. Pop the oldest ready instruction from the OpClass's heap
    2. If squashed: skip, update age ordering
    3. Try to acquire a functional unit: fu_pool->getUnit(op_class)
       ├── NoNeedFU:     Instruction needs no FU (e.g., move elimination)
       ├── NoCapableFU:  Mark inst as unsupported; commit may later panic
       ├── NoFreeFU:     All FUs for this class are busy -- skip
       └── FU acquired:  Proceed
    4. Determine execution latency from the FU
    5. If latency == 1:
         Add directly to instsToExecute list
       If latency > 1:
         Schedule FUCompletion event for (latency - 1) cycles later
         If FU is pipelined: free FU next cycle
         If not pipelined: free FU when event fires
    6. Mark instruction as issued
    7. Update age order list position for this OpClass
```

### 12.4 Non-Speculative Instructions

Instructions that cannot execute speculatively (store conditionals, atomics, certain
system instructions) are stored in a separate `nonSpecInsts` map keyed by sequence
number. They are only scheduled when the commit stage grants permission by sending
the instruction's sequence number via `commitInfo.nonSpecSeqNum`.

### 12.5 Memory Instruction Handling

Memory instructions interact with both the IQ and the Memory Dependence Unit:

```
                    ┌──────────────┐
                    │ IQ::insert() │
                    └──────┬───────┘
                           │
                    ┌──────▼───────┐
                    │ memDepUnit.  │
                    │ insert()     │──▶ Check StoreSet predictor
                    └──────┬───────┘    for memory dependencies
                           │
               ┌───────────┼───────────┐
               ▼                       ▼
        No memory dep            Has memory dep
               │                       │
               ▼                       ▼
        addIfReady()            Wait for store
        (if regs ready)         to issue/complete
               │                       │
               ▼                       ▼
        readyInsts[MemRead]     Later: addIfReady()
```

Three special lists manage memory instruction lifecycle:

- **`deferredMemInsts`**: Instructions waiting for TLB translation to complete (hardware
  page table walk).
- **`blockedMemInsts`**: Instructions blocked because the D-cache port rejected their
  request (cache busy).
- **`retryMemInsts`**: Instructions ready to retry after the cache becomes available.

### 12.6 Squash Handling

When a squash occurs, `doSquash()`
([inst_queue.cc](src/cpu/o3/inst_queue.cc)) walks the per-thread
instruction list from the tail backwards:

1. For each instruction younger than the squash point:
   - Remove from the dependency graph (both producer and consumer entries).
   - Remove from the non-speculative map if applicable.
   - Mark as squashed in the IQ.
2. Clear the dependency graph head nodes for destination registers.

### 12.7 IQ Internal Scoreboard

The IQ maintains its **own** scoreboard separate from the rename stage's scoreboard:

```
Rename Scoreboard:  Updated at rename time (unsetReg) and writeback time (setReg)
                    Used by rename stage to check source readiness

IQ Scoreboard:      Updated within IQ when instructions complete (wakeDependents)
                    Used by IQ to check late-arriving readiness during insertion
```

This dual-scoreboard design accounts for timing differences: a register might be marked
ready in the rename scoreboard but the IQ hasn't yet processed the wakeup signal.

---

## 13. Load/Store Queue (LSQ)

The LSQ ([lsq.hh](src/cpu/o3/lsq.hh), [lsq.cc](src/cpu/o3/lsq.cc),
[lsq_unit.hh](src/cpu/o3/lsq_unit.hh), [lsq_unit.cc](src/cpu/o3/lsq_unit.cc))
handles all memory operations, enforcing memory ordering, implementing store-to-load
forwarding, and managing the interface to the data cache.

### 13.1 Two-Level Architecture

```
┌───────────────────────────────────────────────┐
│ LSQ (top level)                                │
│                                                │
│  ┌────────────┐  ┌────────────┐               │
│  │ LSQUnit[0] │  │ LSQUnit[1] │  ...          │
│  │ (thread 0) │  │ (thread 1) │               │
│  └────────────┘  └────────────┘               │
│                                                │
│  D-Cache Port (shared)                         │
│  SMT sharing policy (Dynamic/Partitioned/      │
│                       Threshold)               │
└───────────────────────────────────────────────┘
```

### 13.2 Load Queue and Store Queue

Each `LSQUnit` contains separate circular queues for loads and stores:

```
Load Queue (CircularQueue<LQEntry>):
┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐
│  L0 │  L1 │  L2 │  L3 │  L4 │  L5 │  L6 │  L7 │
│valid│valid│valid│     │     │     │     │     │
└──┬──┴──┬──┴──┬──┴─────┴─────┴─────┴─────┴─────┘
   head        tail

Store Queue (CircularQueue<SQEntry>):
┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐
│  S0 │  S1 │  S2 │  S3 │  S4 │  S5 │  S6 │  S7 │
│wb'd │wb'd │canWB│     │     │     │     │     │
└──┬──┴─────┴──┬──┴─────┴─────┴─────┴─────┴─────┘
   head   storeWBIt                  tail
```

Each store queue entry (`SQEntry`) extends the base entry with:
- An inline data buffer for the store value
- A `canWB` flag (set when the store is committed and eligible for writeback)
- A `committed` flag
- A `completed` flag (set when the memory system acknowledges the write)

### 13.3 Store-to-Load Forwarding

When a load executes, the `read()` method
([lsq_unit.cc](src/cpu/o3/lsq_unit.cc)) scans the store queue
for potential forwarding:

```
For each older store (from youngest to oldest, down to storeWBIt):

    Compute address overlap between load and store:

    ┌──────────────────────┐     ┌──────────────────────┐
    │ Load range           │     │ Store range                  │
    │ [req_s ─── req_e]    │     │ [st_s ──── st_e]             │
    └──────────────────────┘     └──────────────────────┘

    Three cases:
    ┌───────────────────────────────────────────────────────────┐
    │ Full coverage:   Store fully covers load address range    │
    │                  → Forward data directly from store entry │
    │                  → Schedule writeback event (1-cycle lat) │
    ├───────────────────────────────────────────────────────────┤
    │ Partial coverage: Store partially overlaps load range     │
    │                  → Stall the load                         │
    │                  → Replay when store completes writeback  │
    ├───────────────────────────────────────────────────────────┤
    │ No coverage:     No address overlap                       │
    │                  → Continue scanning older stores         │
    └───────────────────────────────────────────────────────────┘

    If no store forwards data:
        Send request to D-cache via DcachePort
```

### 13.4 Memory Ordering Violation Detection

After both loads and stores execute, the `checkViolations()` method
([lsq_unit.cc](src/cpu/o3/lsq_unit.cc)) checks for ordering
violations:

```
When a STORE executes and computes its address:
    Walk the load queue from the store's insertion point to the tail
    For each YOUNGER load with a valid address:
        If addresses overlap:
            ──▶ VIOLATION: The load may have read stale data
            ──▶ Set memDepViolator = violating load
            ──▶ IEW initiates squash including the violating load
```

### 13.5 Store Writeback to Memory

Stores are written to the memory system only after they are committed. The
`writebackStores()` method
([lsq_unit.cc](src/cpu/o3/lsq_unit.cc)) iterates from `storeWBIt`
forward:

```
For each committed, writeback-eligible store:
    1. Copy data from SQ entry to packet
    2. Build cache request packet
    3. Send to D-cache
    4. If successful:
       a. Unstall any load waiting on this store
       b. Mark store as completed
       c. Under TSO: set storeInFlight = true (only one at a time)
    5. If cache rejects (busy):
       Set isStoreBlocked = true, retry later
```

Under the TSO memory model (`needsTSO = true`), at most one store can be in-flight
to the memory system at any time.

### 13.6 External Snoop Handling

When an external invalidation (cache coherence snoop) arrives, `checkSnoop()`
([lsq_unit.cc](src/cpu/o3/lsq_unit.cc)) scans the load queue:

- Loads whose addresses match the invalidated address may have read stale data.
- Under TSO, all subsequent loads after a snooped load are also marked for squash.
- The load is marked for re-execution (`ReExec` fault).

### 13.7 Split (Unaligned) Operations

The LSQ supports memory accesses that cross cache-line boundaries through the
`SplitDataRequest` class ([lsq.hh](src/cpu/o3/lsq.hh)):

1. The access is split into a prefix fragment (unaligned head), zero or more aligned
   middle fragments, and a suffix fragment.
2. Each fragment is translated independently through the TLB.
3. Fragment responses are reassembled into a single combined response.
4. Cache line crossing is transparent to the rest of the pipeline.

---

## 14. Functional Unit Pool

The FU Pool ([fu_pool.hh](src/cpu/o3/fu_pool.hh),
[fu_pool.cc](src/cpu/o3/fu_pool.cc)) manages the simulated processor's functional
units -- the hardware that actually performs arithmetic, logic, and other operations.

### 14.1 Organization

```
┌──────────────────────────────────────────────────────┐
│ FUPool                                               │
│                                                      │
│  Capability Bitset: [IntAlu, IntMult, FpAlu, MemRW]  │
│                                                      │
│  Per-OpClass Circular Queues:                        │
│    IntAlu:  ──▶ [FU0, FU1, FU2, FU3] ──▶ (wrap)      │
│    IntMult: ──▶ [FU4, FU5] ──▶ (wrap)                │
│    FpAlu:   ──▶ [FU6, FU7] ──▶ (wrap)                │
│    MemRead: ──▶ [FU8, FU9] ──▶ (wrap)                │
│    MemWrite:──▶ [FU8, FU9] ──▶ (wrap)                │
│                                                      │
│  Busy Vector: [0,0,1,0,0,0,1,0,0,0]                  │
│                                                      │
│  Deferred Free: [FU2 (free next cycle)]              │
└──────────────────────────────────────────────────────┘
```

### 14.2 FU Allocation Protocol

When the IQ scheduler needs a functional unit:

```
getUnit(OpClass) returns:
    NoNeedFU  (-3): Instruction doesn't need an FU (NOP-like)
    NoCapableFU(-2): No FU in the pool can handle this OpClass
    NoFreeFU  (-1): All capable FUs are currently busy
    ≥ 0           : FU index allocated; marked busy
```

The circular queue ensures fair distribution across FUs of the same type.

### 14.3 Pipelining and Latency

Each functional unit has two key properties:

- **Operation latency**: How many cycles the operation takes.
- **Pipelined**: Whether the FU can accept a new operation each cycle.

For a pipelined FU with latency 4, the FU is freed after 1 cycle (it can accept new
work), but the result is not available until 4 cycles later. For a non-pipelined FU,
it remains busy for the entire latency period.

### 14.4 Deferred Freeing

FUs are freed through a deferred mechanism:
1. `freeUnitNextCycle(fu_idx)` adds the FU to the `unitsToBeFreed` list.
2. At the start of the next cycle, `processFreeUnits()` clears the busy flag for
   all deferred FUs.

This ensures that an FU freed on cycle N is available for scheduling on cycle N+1.

---

## 15. Reorder Buffer (ROB)

The ROB ([rob.hh](src/cpu/o3/rob.hh), [rob.cc](src/cpu/o3/rob.cc)) maintains
program order by holding all in-flight instructions from rename to commit. It is the
structure that enables precise exceptions in an out-of-order processor.

### 15.1 Data Structure

```
ROB (per-thread doubly-linked lists):

Thread 0:  [inst A] ←→ [inst B] ←→ [inst C] ←→ [inst D]
              ▲ head                               ▲ tail

Thread 1:  [inst X] ←→ [inst Y] ←→ [inst Z]
              ▲ head                    ▲ tail

Global pointers:
  head ──▶ inst A (oldest across all threads, by seqNum)
  tail ──▶ inst D (youngest across all threads, by seqNum)
```

The ROB uses `std::list<DynInstPtr>` per thread, providing O(1) insertion at the tail
and O(1) removal at the head.

### 15.2 SMT Sharing Policies

```
┌─────────────────────────────────────────────────────────────┐
│ Policy       │ Behavior                                     │
├─────────────────────────────────────────────────────────────┤
│ Dynamic      │ All entries shared. Any thread can use all   │
│              │ entries. First-come, first-served.           │
├─────────────────────────────────────────────────────────────┤
│ Partitioned  │ Entries divided equally among threads.       │
│              │ Each thread has a fixed maximum.             │
├─────────────────────────────────────────────────────────────┤
│ Threshold    │ Each thread has a configurable maximum cap.  │
│             │ Cap applies while multiple threads are active;│
│              │ with one active thread, resetEntries() can   │
│              │ expand that thread to full ROB capacity.     │
└─────────────────────────────────────────────────────────────┘
```

### 15.3 Instruction Insertion

When the commit stage calls `insertInst()`
([rob.cc](src/cpu/o3/rob.cc)):

1. The instruction is appended to `instList[tid]` with `push_back()`.
2. If this is the first instruction in the entire ROB, the global `head` is set.
3. The global `tail` is always updated to point at the newly inserted instruction.
4. The instruction is marked as "in ROB" (`inst->setInROB()`).
5. Entry counts are incremented.

### 15.4 Instruction Retirement

When `retireHead()` is called
([rob.cc](src/cpu/o3/rob.cc)):

1. The head instruction is extracted from the thread's list.
2. It must be `readyToCommit()` (assertion).
3. Entry counts are decremented.
4. The instruction's "in ROB" flag is cleared and "committed" flag is set.
5. `updateHead()` scans all threads to find the new globally oldest instruction.

### 15.5 Multi-Cycle Squashing

ROB squashing may take multiple cycles, bounded by the configurable `squashWidth`
parameter. The `doSquash()` method
([rob.cc](src/cpu/o3/rob.cc)) uses a persistent per-thread iterator
(`squashIt[tid]`) to remember where it left off:

```
Cycle 1: squash(seqNum=100, tid=0)
         Squash instructions 120, 119, 118, 117 (squashWidth = 4)

Cycle 2: doSquash(tid=0) continues
         Squash instructions 116, 115, 114, 113

Cycle 3: doSquash(tid=0) continues
         Squash instructions 112, 111, 110, 109
         Reached seqNum 100 → doneSquashing = true
```

Each squashed instruction is marked as squashed AND marked as `canCommit`, so it can
drain through the ROB head during the commit stage's normal retirement loop.

---

## 16. Commit Stage

The Commit stage ([commit.hh](src/cpu/o3/commit.hh),
[commit.cc](src/cpu/o3/commit.cc)) is the final pipeline stage. It retires
instructions in strict program order from the ROB head, making speculative results
architecturally visible.

### 16.1 The Commit Loop

The `commitInsts()` method
([commit.cc](src/cpu/o3/commit.cc)) is the core retirement loop:

```
While num_committed < commitWidth:

    1. SELECT THREAD (via SMT commit policy)

    2. HANDLE PENDING INTERRUPT (if any)
       Wait for pipeline to drain, then process interrupt

    3. CHECK ROB HEAD
       If not ready: break (nothing to commit)

    4. IF HEAD IS SQUASHED
       ├── Retire it (does NOT count toward commit bandwidth)
       └── Continue to next instruction

    5. ATTEMPT COMMIT via commitHead()
       ├── Success: increment num_committed, update rename maps
       └── Failure: instruction not yet executed → break
```

### 16.2 commitHead() -- The Three Cases

The `commitHead()` method
([commit.cc](src/cpu/o3/commit.cc)) handles three cases:

```
┌─────────────────────────────────────────────────────────────────┐
│ Case 1: Instruction Not Yet Executed                            │
│                                                                 │
│  Applies to: non-speculative insts, atomics, barriers,          │
│              strictly ordered loads, store conditionals         │
│                                                                 │
│  Action: Signal IEW to schedule this instruction now that it    │
│          is at the ROB head. Clear canCommit so it won't be     │
│          re-attempted until it executes.                        │
│  Returns: false                                                 │
├─────────────────────────────────────────────────────────────────┤
│ Case 2: Instruction Has a Fault                                 │
│                                                                 │
│  Precondition: Must be the first instruction committed this     │
│                cycle AND all stores must have written back.     │
│                                                                 │
│  Action: Invoke the fault handler via cpu->trap().              │
│          Generate a trap event to squash the pipeline after     │
│          trapLatency cycles. Set status to TrapPending.         │
│  Returns: false                                                 │
├─────────────────────────────────────────────────────────────────┤
│ Case 3: Normal Successful Commit                                │
│                                                                 │
│  Action: Update committed rename map with final register        │
│          mappings. Retire instruction from ROB.                 │
│          Apply deferred misc register writes.                   │
│          Record commit timestamp on instruction.                │
│  Returns: true                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 16.3 Trap and Interrupt Processing

```
Trap Flow:
                                                ┌──────────────────┐
  Faulting instruction ──▶ commitHead() ──▶    │ cpu->trap(fault)  │
  at ROB head                                   │ (invoke handler)  │
                                                └────────┬─────────┘
                                                         │
                                          generateTrapEvent()
                                                         │
                                          Schedule event at
                                          curTick + trapLatency
                                                         │
                                                         ▼
                                          processTrapEvent()
                                          (sets trapSquash[tid])
                                                         │
                                                         ▼
                                          squashFromTrap()
                                          (squashAll + redirect
                                           to fault handler PC)

Interrupt Flow:
  propagateInterrupt()    ──▶ Tell fetch: interruptPending = true
                               (fetch stalls)
  handleInterrupt()       ──▶ Wait for pipeline to drain completely
                               (cpu->instList.empty())
                          ──▶ cpu->processInterrupts()
                          ──▶ generateTrapEvent()
                          ──▶ squash pipeline
```

### 16.4 Committed Rename Map Update

At commit time, for each destination register of the committed instruction, the
**committed rename map** is updated
([commit.cc](src/cpu/o3/commit.cc)):

```
For each destination register:
    commitRenameMap[tid].setEntry(flattenedArchDest, renamedPhysDest)
```

This keeps the committed rename map in sync with the actual architectural state,
ensuring that external register reads (e.g., from a debugger or the OS) return the
correct values.

### 16.5 SMT Commit Policies

```
┌─────────────────────────────────────────────────────────────┐
│ Policy       │ Algorithm                                    │
├─────────────────────────────────────────────────────────────┤
│ RoundRobin   │ Rotate through threads. Pick the first       │
│              │ thread with a ready ROB head. Move it to     │
│              │ the back of the priority list.               │
├─────────────────────────────────────────────────────────────┤
│ OldestReady  │ Scan all threads. Pick the thread whose      │
│              │ ROB head has the globally smallest (oldest)  │
│              │ sequence number.                             │
├─────────────────────────────────────────────────────────────┤
│ (Special)    │ Exiting threads take priority over all       │
│              │ policies. An exiting thread's ROB is drained │
│              │ first to ensure clean thread removal.        │
└─────────────────────────────────────────────────────────────┘
```

### 16.6 Per-Thread Status Machine

```
┌─────────┐   commit inst   ┌──────────────────┐
│ Running │ ◀─────────────▶ │ FetchTrapPending │
└────┬────┘                 └──────────────────┘
     │                               ▲
     │ fault at head                 │ trap event fires
     ▼                               │
┌─────────────┐                ┌─────┴────────┐
│ TrapPending │ ─────────────▶ │ ROBSquashing │
└─────────────┘   squash all   └──────────────┘
                                     ▲
                                     │ squash from IEW or SquashAfter
                                     │
                        ┌────────────┴─────────────┐
                        │    SquashAfterPending    │
                        └──────────────────────────┘
                      (committed a SquashAfter inst;
                       will squash next cycle)
```

---

## 17. Squash Mechanism

Squashing is the process of discarding speculatively executed instructions when
speculation turns out to be wrong (branch misprediction, memory ordering violation,
trap, or external state change). The O3CPU has a sophisticated, prioritized squash
system.

### 17.1 Squash Sources

```
┌──────────────────────────────────────────────────────────────────────────┐
│ Source             │ Detected At │ Signal Path                           │
├──────────────────────────────────────────────────────────────────────────┤
│ Branch mispredict  │ IEW execute │ IEW → Commit → All stages             │
│                    │             │ (includeSquashInst = false)           │
├──────────────────────────────────────────────────────────────────────────┤
│ Mem order violation│ IEW execute │ IEW → Commit → All stages             │
│                    │             │ (includeSquashInst = true)            │
├──────────────────────────────────────────────────────────────────────────┤
│ Direct branch      │ Decode      │ Decode → Fetch (direct redirect)      │
│ target mismatch    │             │ Also Decode → CPU (remove insts)      │
├──────────────────────────────────────────────────────────────────────────┤
│ Trap/Fault         │ Commit      │ Commit → All stages (squashAll)       │
├──────────────────────────────────────────────────────────────────────────┤
│ Interrupt          │ Commit      │ Commit → All stages (squashAll)       │
├──────────────────────────────────────────────────────────────────────────┤
│ External TC write  │ Commit      │ Commit → All stages (squashAll)       │
├──────────────────────────────────────────────────────────────────────────┤
│ SquashAfter inst   │ Commit      │ Commit → All stages (next cycle)      │
├──────────────────────────────────────────────────────────────────────────┤
│ Drain request      │ Commit      │ Commit → All stages (squashAfter)     │
└──────────────────────────────────────────────────────────────────────────┘
```

### 17.2 Squash Priority

When multiple squash sources fire simultaneously, priority is determined by instruction
age (older squashes override younger ones):

```
Priority (highest to lowest):
  1. Trap/Interrupt at Commit (oldest possible -- at ROB head)
  2. TC write at Commit
  3. SquashAfter at Commit
  4. IEW-initiated squash (branch mispredict or memory violation)
     └── Memory violation overrides branch mispredict for same instruction
  5. Decode-initiated squash (direct branch target mismatch)
```

### 17.3 Squash Propagation Flow

For an IEW-initiated squash (the most common case):

```
Cycle N:   IEW detects misprediction
           ├── squashDueToBranch() writes to toCommit:
           │     squash=true, squashedSeqNum, corrected PC
           │
           ▼
Cycle N+1: Commit receives squash signal from IEW
           ├── commit() processes squash:
           │     Set commitStatus = ROBSquashing
           │     rob->squash(squashedSeqNum, tid)
           │     Broadcast to all stages via toIEW->commitInfo:
           │       squash=true, robSquashing=true, doneSeqNum, pc
           │
           ▼
Cycle N+2: Each stage receives squash at its configured delay:
           │
           ├── Fetch (commitToFetchDelay cycles later):
           │     doSquash() → reset PC, clear fetch queue, invalidate I-cache req
           │
           ├── Decode (commitToDecodeDelay cycles later):
           │     squash() → clear internal queues, unblock fetch
           │
           ├── Rename (commitToRenameDelay cycles later):
           │     squash() → undo history buffer, clear queues
           │     doSquash() → restore rename map entries
           │     Deferred register freeing → freeingInProgress
           │
           └── IEW (commitToIEWDelay cycles later):
                 squash() → IQ.squash(), LSQ.squash()
                 Clear skid buffer, discard rename input

Cycle N+3+: ROB continues squashing (up to squashWidth per cycle)
            rob->doSquash() removes entries incrementally
            When done: commitStatus → Running
```

### 17.4 Per-Stage Squash Actions

```
┌───────────────────────────────────────────────────────────────────────┐
│ Stage   │ Squash Actions                                              │
├───────────────────────────────────────────────────────────────────────┤
│ BAC     │ Revert branch predictor speculative state                   │
│         │ In decoupled mode, squash/clear FTQ bookkeeping             │
├───────────────────────────────────────────────────────────────────────┤
│ Fetch   │ Reset PC to squash target                                   │
│         │ Clear fetch queue                                           │
│         │ Invalidate pending I-cache requests                         │
│         │ Reset ISA decoder                                           │
├───────────────────────────────────────────────────────────────────────┤
│ Decode  │ Clear instruction and skid buffers                          │
│         │ Unblock fetch if decode was blocking                        │
├───────────────────────────────────────────────────────────────────────┤
│ Rename  │ Walk history buffer, undo speculative renames               │
│         │ Restore old register mappings in rename map                 │
│         │ Defer freeing of new registers (SMT safety)                 │
│         │ Clear instruction and skid buffers                          │
│         │ Unblock decode if rename was blocking                       │
├───────────────────────────────────────────────────────────────────────┤
│ IEW     │ Squash IQ entries (remove dependency graph links)           │
│         │ Squash LSQ entries (remove load/store queue entries)        │
│         │ Clear skid buffer                                           │
│         │ Discard incoming instructions from rename                   │
│         │ Unblock rename if IEW was blocking                          │
├───────────────────────────────────────────────────────────────────────┤
│ Commit  │ Initiate ROB squash (may take multiple cycles)              │
│         │ Set commitStatus = ROBSquashing                             │
│         │ Broadcast squash signals to all stages                      │
└───────────────────────────────────────────────────────────────────────┘
```

---

## 18. Memory Dependence Prediction

The O3CPU uses the **Store Set** algorithm (Chrysos and Emer, 1998) to predict which
loads depend on which stores, preventing unnecessary stalls while avoiding memory
ordering violations.

### 18.1 Core Concept

When a memory ordering violation is detected (a load read data from memory before an
older store wrote to the same address), the load and store are placed in the same
"store set." Future instances of the load will wait for the most recent store in the
same set before executing.

### 18.2 Data Structures

```
┌───────────────────────────────────────────────────────────────┐
│ Store Set Predictor                                           │
│                                                               │
│  SSIT (Store Set ID Table):                                   │
│    Set-associative cache mapping instruction PCs to SSIDs     │
│    ┌──────┬──────┬──────┬──────┐                              │
│    │ PC_A │ PC_B │ PC_C │ ...  │  ──▶ SSID                    │
│    │ =5   │ =12  │ =5   │      │                              │
│    └──────┴──────┴──────┴──────┘                              │
│                                                               │
│  LFST (Last Fetched Store Table):                             │
│    Direct-mapped table: SSID → last fetched store seqNum      │
│    ┌──────┬──────┬──────┬──────┐                              │
│    │SSID 0│SSID 1│SSID 5│ ...  │                              │
│    │ =--- │ =--- │ =42  │      │                              │
│    └──────┴──────┴──────┴──────┘                              │
│                                                               │
│  storeList: Map of in-flight stores (seqNum → SSID)           │
└───────────────────────────────────────────────────────────────┘
```

### 18.3 Prediction Flow

```
1. Load/Store INSERTION:
   ├── Look up instruction PC in SSIT
   ├── If valid SSID found:
   │     Read LFST[SSID] → get last store's sequence number
   │     Load must wait for that store before executing
   └── If no SSID: instruction has no predicted dependency

2. Store ISSUED:
   ├── Remove from storeList
   └── If last store in SSID: invalidate LFST entry

3. VIOLATION detected:
   ├── Both have SSIDs: merge (smaller wins)
   ├── One has SSID: assign to the other
   ├── Neither has SSID: create new SSID from load PC
   └── Update both load and store SSIT entries

4. PERIODIC CLEAR:
   Every clearPeriod memory operations, wipe all state
   to prevent predictor saturation from old patterns
```

### 18.4 Integration with Memory Dependence Unit

The `MemDepUnit` ([mem_dep_unit.hh](src/cpu/o3/mem_dep_unit.hh),
[mem_dep_unit.cc](src/cpu/o3/mem_dep_unit.cc)) wraps the Store Set predictor and
manages per-instruction memory dependency tracking:

- Each memory instruction gets a `MemDepEntry` that tracks: whether its registers are
  ready, how many memory dependencies remain, and a list of dependent instructions.
- Memory barriers create explicit dependencies on all subsequent memory operations.
- When a store completes, the MemDepUnit wakes all loads that were waiting on it.

---

## 19. Simultaneous Multithreading (SMT)

The O3CPU supports up to `MaxThreads` (4) hardware threads sharing a single pipeline.
SMT support is pervasive throughout the design.

### 19.1 Shared vs. Per-Thread Resources

```
┌────────────────────────────────────────────────────────────────┐
│ SHARED Resources             │ PER-THREAD Resources            │
├────────────────────────────────────────────────────────────────┤
│ Physical register file       │ Rename maps                     │
│ Unified free lists           │ Fetch queue                     │
│ Instruction queue (IQ)       │ Fetch buffer                    │
│ Functional unit pools        │ PC state                        │
│ ROB entries (policy-based)   │ ISA decoder                     │
│ D-cache port                 │ Thread status                   │
│ I-cache port                 │ Skid buffers (per stage)        │
│ Pipeline bandwidth           │ LSQ entries (policy-based)      │
│ Scoreboard                   │ History buffer                  │
└────────────────────────────────────────────────────────────────┘
```

### 19.2 Thread Selection Policies

Each stage that must select among threads has its own policy:

```
┌──────────┬──────────────────────────────────────────────────────┐
│ Stage    │ Policies Available                                   │
├──────────┼──────────────────────────────────────────────────────┤
│ Fetch    │ RoundRobin, IQCount, LSQCount, Branch                │
│          │ (configurable via SMTFetchPolicy)                    │
├──────────┼──────────────────────────────────────────────────────┤
│ Decode   │ No separate SMT policy object                        │
│          │ Iterates active threads; decodeWidth shared globally │
├──────────┼──────────────────────────────────────────────────────┤
│ Rename   │ No separate SMT policy object                        │
│          │ Width shared globally across per-thread input queues │
├──────────┼──────────────────────────────────────────────────────┤
│ IEW      │ Width shared across threads                          │
│ (dispatch) │                                                    │
├──────────┼──────────────────────────────────────────────────────┤
│ IEW      │ Oldest-first across all threads (by seqNum)          │
│ (issue)  │                                                      │
├──────────┼──────────────────────────────────────────────────────┤
│ Commit   │ RoundRobin or OldestReady                            │
│          │ (configurable via CommitPolicy)                      │
└──────────┴──────────────────────────────────────────────────────┘
```

### 19.3 Resource Sharing Policies

The ROB, LSQ, and each IQUnit support `Dynamic`, `Partitioned`, and `Threshold`
policies:

- **Dynamic**: All entries form a single shared pool. Any thread can use any entry.
  Maximizes utilization but allows one thread to starve others.
- **Partitioned**: Entries are divided into per-thread shares (typically equal shares).
  Guarantees minimum allocation but may waste entries when threads have unequal demand.
- **Threshold**: Each thread is limited by a configured cap for that structure. The
  implementation enforces per-thread maxima directly; it does not maintain a separate
  explicit "overflow pool" object.

### 19.4 Per-Thread Fetch Queues

A critical SMT design choice is the use of per-thread fetch queues
([fetch.hh](src/cpu/o3/fetch.hh)). Without per-thread queues, a stalled
thread's instructions would create head-of-line blocking for other threads. The
per-thread design allows each thread to buffer independently, and the drain-to-decode
logic interleaves across threads with a random starting point for fairness.

---

## 20. CPU Lifecycle and Drain Support

### 20.1 Activity-Driven Scheduling

The O3CPU uses an activity recorder to avoid simulating cycles when the pipeline is
completely idle:

```
Each cycle:
    1. Each stage reports activity via activityThisCycle()
       or activateStage()/deactivateStage()
    2. At end of tick(), the CPU checks activityRec.active()
    3. If inactive: do NOT schedule next tick event
       (CPU sleeps until woken by external event)
    4. If active: schedule next tick at clockEdge(Cycles(1))
```

Stages activate themselves when they have internal work (e.g., unblocking a skid buffer)
even if no new instructions are flowing.

### 20.2 CPU Status Model

```
┌─────────┐   activateContext()   ┌─────────┐
│  Idle   │ ────────────────────▶ │ Running │
└─────────┘                       └────┬────┘
     ▲                                 │
     │ no active threads               │ switchOut()
     │                                 ▼
     │                           ┌────────────┐
     │                           │ SwitchedOut │
     │                           └────────────┘
     │
     │ suspendContext()
     └─────────────────────────────────────────
```

### 20.3 Drain Protocol

Draining is the process of bringing the CPU to a quiescent state for checkpointing,
CPU switching, or simulation termination:

```
1. drain() called
   ├── Set drainPending = true
   ├── commit.drain() begins
   └── Wake CPU if sleeping

2. Commit waits for a clean instruction boundary:
   ├── Not mid-microop (microPC == 0)
   ├── No trap or interrupt pending
   └── Then: squashAfter() to flush pipeline
       cpu->commitDrained() to stall front-end

3. Pipeline drains naturally:
   ├── All instructions flow to commit and retire
   ├── All stores write back to memory
   └── All queues empty

4. tryDrain() checks isCpuDrained():
   ├── instList/removeList empty?
   └── Stage-level drained checks pass (BAC/Fetch/Decode/Rename/IEW/Commit)

5. When fully drained:
   ├── Deschedule tick event
   ├── Signal DrainState::Drained
   └── CPU is now quiescent
```

### 20.4 Instruction List Management

The CPU maintains a global list of all in-flight instructions (`instList`). This list
provides a definitive view of every instruction in the pipeline, independent of which
stage "owns" it. Key operations:

- **`addInst()`**: Called at fetch time. Appends to the list and returns an iterator.
- **`removeFrontInst()`**: Called at commit time. Marks the instruction for removal.
- **`removeInstsNotInROB()`**: Called on squash from commit. Marks all non-ROB
  instructions for removal.
- **`removeInstsUntil()`**: Called on squash from decode. Marks instructions with
  sequence numbers beyond a threshold for removal.
- **`cleanUpRemovedInsts()`**: Called at the end of each `tick()`. Actually erases marked
  instructions from the list.

The deferred removal pattern (mark then erase) prevents iterator invalidation during
the cycle when multiple stages may be referencing the same instructions.

---

## 21. Statistics and Observability

### 21.1 Per-Stage Statistics

Each pipeline stage tracks its own set of statistics:

```
Fetch:   cycles per thread status, predicted branches, cache lines fetched,
         I-cache squashes, TLB squashes, instructions fetched per cycle distribution

Decode:  cycles per thread status, branches resolved, branch mispredictions detected,
         control mispredictions, decoded instructions, squashed instructions

Rename:  cycles per thread status, renamed instructions, squashed instructions,
         ROB/IQ/LQ/SQ full events, register type lookups, committed/undone maps,
         serialization events

IEW:     cycles per dispatch/exec status, dispatched instructions (total, load, store,
         non-spec), IQ/LSQ full events, memory ordering violations,
         branch mispredictions (taken/not-taken), writeback counts, producer/consumer
         fanout

Commit:  cycles per thread status, squashed instructions retired, non-spec stalls,
         branch mispredictions, committed instructions per cycle distribution,
         committed atomics and barriers, committed instruction types by OpClass
```

### 21.2 Probe Points

The O3CPU exposes probe points that external observers can attach to:

```
Fetch:    ppFetch (instruction fetched), ppFetchRequestSent (I-cache request)
IEW:      ppMispredict (misprediction detected), ppDispatch (instruction dispatched),
          ppExecute (execution begins), ppToCommit (execution complete)
Commit:   ppCommit (instruction committed), ppCommitStall (ROB head not ready),
          ppSquash (squashed instruction retired)
FTQ:      ppFTQInsert (fetch target inserted), ppFTQRemove (fetch target removed)
```

### 21.3 Pipeline Visualization

The `O3PipeView` tracing facility uses per-instruction tick stamps to generate pipeline
diagrams. Each instruction's destructor
([dyn_inst.cc](src/cpu/o3/dyn_inst.cc)) emits a trace record with
its timestamps through each stage, enabling offline reconstruction of the pipeline state.

---

## 22. Configuration and Parameterization

### 22.1 Key Parameters

The O3CPU is configured through Python SimObject parameters defined in
[BaseO3CPU.py](src/cpu/o3/BaseO3CPU.py):

```
Pipeline Width Parameters:
    fetchWidth          Instructions fetched per cycle
    decodeWidth         Instructions decoded per cycle
    renameWidth         Instructions renamed per cycle
    dispatchWidth       Instructions dispatched to IQ per cycle
    issueWidth          Instructions issued from IQ per cycle
    wbWidth             Instructions written back per cycle
    commitWidth         Instructions committed per cycle

Pipeline Depth Parameters:
    fetchToDecodeDelay       Fetch → Decode latency (cycles)
    decodeToRenameDelay      Decode → Rename latency
    renameToIEWDelay         Rename → IEW latency
    issueToExecuteDelay      IQ issue → FU execute latency
    iewToCommitDelay         IEW → Commit latency
    commitToIEWDelay         Commit → IEW backward signal latency
    commitToFetchDelay       Commit → Fetch backward signal latency
    (and other backward delays)

Buffer Sizes:
    numROBEntries            Total ROB entries
    instQueues               Vector of IQUnit SimObjects
    instQueues[*].numEntries Entries per IQUnit
    LQEntries                Base load-queue entry count (policy dependent)
    SQEntries                Base store-queue entry count (policy dependent)
    fetchBufferSize          Fetch buffer size (bytes)
    fetchQueueSize           Fetch queue entries (instructions)

Register File:
    numPhysIntRegs           Physical integer registers
    numPhysFloatRegs         Physical floating-point registers
    numPhysVecRegs           Physical vector registers
    numPhysVecPredRegs       Physical vector predicate registers
    numPhysMatRegs           Physical matrix registers
    numPhysCCRegs            Physical condition code registers

SMT:
    numThreads               Number of hardware threads
    smtNumFetchingThreads    Threads fetch can service per cycle
    smtFetchPolicy           Fetch thread selection policy
    smtCommitPolicy          Commit thread selection policy
    smtROBPolicy             ROB sharing policy
    smtROBThreshold          ROB threshold policy cap
    smtLSQPolicy             LSQ sharing policy
    smtLSQThreshold          LSQ threshold policy cap
    instQueues[*].smtIQPolicy    IQ sharing policy (per IQUnit)
    instQueues[*].smtIQThreshold IQ threshold parameter in percent

Branch Prediction:
    decoupledFrontEnd        Enable decoupled front-end mode
    numFTQEntries            FTQ size (decoupled front-end mode)
    fetchTargetWidth         Max bytes per fetch target
    minInstSize              BTB scan granularity in BAC
    maxFTPerCycle            Max fetch targets per cycle
    maxTakenPredPerCycle     Max taken predictions per cycle

Miscellaneous:
    trapLatency              Cycles to process a trap
    squashWidth              Optional ROB squash bandwidth limit
                              (unspecified = squash all in one cycle)
```

### 22.2 Typical Configuration Examples

A simple in-order-like configuration (narrow pipeline):

```
fetchWidth = 1, decodeWidth = 1, renameWidth = 1
dispatchWidth = 1, issueWidth = 1, commitWidth = 1
numROBEntries = 16
instQueues = [IQUnit(numEntries=8)]
LQEntries = 8, SQEntries = 8
numPhysIntRegs = 64
```

A wide out-of-order configuration:

```
fetchWidth = 8, decodeWidth = 8, renameWidth = 8
dispatchWidth = 8, issueWidth = 8, commitWidth = 8
numROBEntries = 256
instQueues = [IQUnit(numEntries=128)]
LQEntries = 64, SQEntries = 64
numPhysIntRegs = 256
```

### 22.3 Functional Unit Configuration

Functional units are configured through `FUPool` SimObjects. Each pool contains a list
of functional unit descriptions, each specifying:

- The operation classes it supports (IntAlu, IntMult, FpAlu, etc.)
- The number of copies
- The operation latency per class
- Whether it is pipelined

The [FuncUnitConfig.py](src/cpu/o3/FuncUnitConfig.py) file provides default functional
unit configurations that can be overridden in simulation scripts.

### 22.4 Debug Flags

The O3-specific debug flags declared in
[SConscript](src/cpu/o3/SConscript) are:

```
O3CPU        General CPU-level messages
BAC          Branch Address Calculation
CommitRate   Per-cycle commit counts
FTQ          Fetch target queue
IEW          Issue/Execute/Writeback
IQ           Instruction queue scheduling
LSQ          Load/Store queue operations
LSQUnit      Per-thread LSQ details
MemDepUnit   Memory dependence tracking
ROB          Reorder buffer operations
Rename       Register renaming
Scoreboard   Register readiness tracking
StoreSet     Store set predictor
Writeback    Writeback events

O3CPUAll     Compound flag enabling:
             BAC, FTQ, Fetch, Decode, Rename, IEW, Commit,
             IQ, ROB, FreeList, LSQ, LSQUnit, StoreSet, MemDepUnit,
             DynInst, O3CPU, Activity, Scoreboard, Writeback
```

---

## Appendix A: Complete Pipeline Data Flow Diagram

```
                    ┌──────────────────────────────────────────────────────────┐
                    │                 O3CPU Pipeline                           │
                    │                                                          │
                    │  ┌─────┐   generateFetchTargets()   ┌───┐                │
                    │  │ BAC │───────────────────────────▶ │FTQ│               │
                    │  └──▲──┘   (decoupled front-end)     └─┬─┘               │
                    │     │                                   │                │
                    │     │ updatePC()                        │                │
                    │     │ (all modes)                       │                │
                    │     │                           readHead()/popHead()     │
                    │     │                           (decoupled mode)         │
                    │     │                                   ▼                │
  I-Cache ◀────────│  ┌───────┐  FetchStruct  ┌────────┐  DecodeStruct         │
                    │  │ Fetch │──────────────▶│ Decode │──────────────▶       │
  ITLB ◀───────────│  └───────┘  (fetchQueue)  └────────┘  (decodeQueue)        │
                    │                                           │              │
                    │                                    RenameStruct          │
                    │                                    (renameQueue)         │
                    │                                           │              │
                    │                                           ▼              │
                    │                           ┌──────────────────────────┐   │
                    │                           │          IEW             │   │
                    │                           │                          │   │
                    │                           │  Dispatch ──▶ IQ ──▶    │    │
                    │                           │              │    ├─▶FU │    │
  D-Cache ◀────────│                           │              │    └─▶LSQ│   │
                    │                           │              │          │    │
                    │                           │  Writeback ◀─┘          │    │
                    │                           └──────────┬───────────────┘   │
                    │                                      │                   │
                    │                               IEWStruct                  │
                    │                               (iewQueue)                 │
                    │                                      │                   │
                    │                                      ▼                   │
                    │                               ┌──────────┐               │
                    │                               │  Commit  │               │
                    │                               │          │               │
                    │                               │  ROB ◀───│               │
                    │                               └──────────┘               │
                    │                                                          │
                    │  ◀──────── Backward Communication (TimeStruct) ────────▶ │
                    │     squash, stall, free entries, interrupt, trap         │
                    └──────────────────────────────────────────────────────────┘
```

## Appendix B: Instruction Lifecycle State Diagram

```
                              ┌─────────────┐
                              │   Created   │
                              └──────┬──────┘
                                     │
                    ┌────────────────┴───────────────┐
                    │                                │
                    ▼                                ▼
             ┌────────────┐                   ┌───────────┐
             │  In Fetch  │                   │ Squashed  │
             │   Queue    │                   └───────────┘
             └─────┬──────┘
                   │
                   ▼
             ┌────────────┐
             │  In Decode │
             │   Buffer   │
             └─────┬──────┘
                   │
                   ▼
             ┌────────────┐
             │  Renamed   │
             │  IqEntry   │
             │  RobEntry  │
             │ [LsqEntry] │
             └─────┬──────┘
                   │
          ┌────────┴────────┐
          │                 │
          ▼                 ▼
    ┌───────────┐     ┌───────────┐
    │ CanIssue  │     │  Waiting  │
    └─────┬─────┘     │  in IQ    │
          │           └─────┬─────┘
          │                 │ wakeup
          └──────────┬──────┘
                     ▼
               ┌───────────┐
               │  Issued   │
               └─────┬─────┘
                     │
                     ▼
               ┌───────────┐
               │ Executed  │
               └─────┬─────┘
                     │
                     ▼
               ┌───────────┐
               │ CanCommit │
               └─────┬─────┘
                     │
                     ▼
               ┌───────────┐
               │ Committed │
               └───────────┘
```

Notes:
- `Created` is at fetch (`buildInst()`).
- `Squashed` may occur from any pipeline stage.
- `CanCommit` waits at the ROB head after writeback.

## Appendix C: Squash Signal Flow Diagram

```
                       Branch Misprediction or Memory Violation
                                        │
                                        ▼
                                 ┌──────────┐
                                 │   IEW    │
                                 │ Execute  │
                                 └────┬─────┘
                                      │ toCommit: squash=true
                                      ▼
                                 ┌──────────┐
                                 │  Commit  │
                                 │ (verify  │
                                 │  priority│
                                 │  by age) │
                                 └────┬─────┘
                                      │ toIEW->commitInfo: squash=true
                                      │                    robSquashing=true
                                      │                    doneSeqNum
                                      │                    pc (redirect)
                                      │
              ┌───────────────────────┼─────────────────────────┬─────────────┐
              │                       │                         │             │
              ▼                       ▼                         ▼             ▼
        ┌──────────┐           ┌──────────┐               ┌──────────┐ ┌────────────┐
        │  Fetch   │           │  Rename  │               │   IEW    │ │ BAC / FTQ  │
        │ doSquash │           │ doSquash │               │  squash  │ │ (decoupled)│
        │          │           │          │               │          │ │            │
        │ Reset PC │           │ Undo     │               │ IQ.squash│ │ squash BPU │
        │ Clear    │           │ renames  │               │ LSQ.squash│ │ histories  │
        │ queue    │           │ Restore  │               │ Clear    │ │ FTQ reset  │
        │ Reset    │           │ map      │               │ skid buf │ │            │
        │ decoder  │           │ Defer    │               │          │ │            │
        │          │           │ reg free │               │          │ │            │
        └────┬─────┘           └──────────┘               └──────────┘ └────────────┘
             │
             ▼
        ┌──────────┐
        │  Decode  │
        │  squash  │
        │          │
        │ Clear    │
        │ buffers  │
        └──────────┘

                        (all squash paths converge)
                                      │
                                      ▼
                            Resume fetching from
                            corrected PC address
```

---

*This document describes the O3CPU model as implemented in the gem5 simulator source
code under `src/cpu/o3/`. Code references point to the corresponding source files
within that directory.*
