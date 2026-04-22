# chi_testbench_gem5_v2

A CPU-less CHI testbench that drives each tile's `CHI_TileCacheController`
from the **CHI-protocol side** instead of the CPU/sequencer side.
The 4x4 Garnet mesh, HN-Fs, SN-Fs, and Misc Node stay the same as in
`chi_testbench_gem5` (v1); what changes is the stimulus source.

## What's different from v1

| | v1 (`chi_testbench_gem5`) | v2 (`chi_testbench_gem5_v2`) |
|---|---|---|
| Driver is a | `ClockedObject` with a `RequestPort` | **Full Ruby network node** (subclass of `CHIGenericController`) |
| Stimulus enters via | `RubySequencer.in_ports` → `mandatoryQueue` | Directly constructed `CHIRequestMsg` on the driver's `reqOut` |
| Driver → tile hop | Internal to the SLICC automaton | Real Garnet traversal (same mesh router, one ExtLink hop) |
| Driver's outbound opcode | Determined by SLICC from the MemCmd flavor | Chosen by the test sequence |
| Snoop reception | Not possible (sequencer side) | Driver responds with `SnpResp_I` to every incoming snoop |
| Handle flow-control | Sequencer BufferFull/recvReqRetry | `MessageBuffer::enqueue` + CompDBIDResp/Comp handshake |

Both testbenches pair identically structured C++ sequence bodies with
Python scenario modules; the API surface exposed to sequences is the
biggest user-visible delta.

### API surface (CHI-inspired, replacing v1's CPU-inspired one)

```cpp
// Blocking
void read_shared(addr, dst, len);
void write_unique_full(addr, src, len);
void write_no_snp_full(addr, src, len);
void clean_unique(addr);          // ownership upgrade, no data

// Non-blocking (async; one-in-flight per line address)
Handle async_read_shared(addr, dst, len);
Handle async_write_unique_full(addr, src, len);
Handle async_write_no_snp_full(addr, src, len);
void   resolve(Handle);
void   resolve_all();

// Time / sync (same shape as v1)
void wait_ticks(Tick);
void wait_cycles(Cycles);
void wait_on(name);
void notify(name);
```

Under the hood each of these builds a `CHIRequestMsg`/`CHIResponseMsg`/
`CHIDataMsg` with the appropriate opcode, routes it through Garnet to
the tile controller's MachineID, and waits on the matching inbound
message on the driver's `rspIn`/`datIn` ports.

## Architecture

Each tile in v2 contains **two** CHI network nodes on the same
mesh router:

```
          ChiDriverNode                          CHI_TileCacheController
       (synthetic upstream RN)                       (RN-F or RN-I)
       ──────────────────────                    ──────────────────────
 Fiber ─▶ reqOut ──┐                                        ┌── reqIn
 sequence snpOut   │                                        │    snpIn
 body     rspOut   ├──▶ Garnet mesh router 0..15 ◀──────────┤    rspIn
          datOut   │        (same router)                   │    datIn
          reqIn ◀──┤                                        ├──▶ reqOut
          snpIn    │                                        │    snpOut
          rspIn    │                                        │    rspOut
          datIn ◀──┘                                        └──  datOut
                            │
                            ▼
                    (mesh links → HN-Fs → SN-Fs)
```

Both controllers ExtLink to the same router because the `CHI_Tile_v2`
wrapper inherits from `CHI_RNI_DMA` (so `CustomMesh` buckets it as
such) and `getNetworkSideControllers()` returns both.

The tile cache controller is the same `Base_CHI_Cache_Controller`
parameterization as in v1, with two differences:

  * `sequencer = NULL` — there is no CPU-side traffic in v2.
  * `send_evictions = False` in `rnf_l2` mode — the eviction callback
    would otherwise poke a nonexistent sequencer's LL/SC monitor.

MachineID allocation is shared: both controllers register as
`MachineType_Cache` and pull their version numbers out of the same
`Versions.getVersion(CHI_Cache_Controller)` counter, so every tile's
driver and cache controller have distinct MachineIDs within the Cache
type (HNFs use the same pool).

## Layout

```
src/chi_testbench_gem5_v2/
├── SConscript                    Build registration (CHI-only)
├── ChiTestbenchGem5V2.py        SimObject defs: ChiDriverNode,
│                                  ChiGem5V2Barrier, ChiGem5V2EventBus
├── driver.hh / driver.cc        ChiDriverNode (CHIGenericController subclass)
├── seq_thread.hh                Fiber wrapper for sequence execution
├── sequence_context.hh          SequenceContext + SequenceFn
├── sync/
│   ├── barrier.hh / .cc         ChiBarrier (counter + exitSimLoop)
│   └── latch.hh / .cc           Latch + ChiEventBus
└── sequences/
    ├── registry.hh / .cc        String → SequenceFn lookup
    ├── idle.cc                  No traffic
    ├── smoke_read.cc            ReadShared walk
    ├── smoke_write.cc           WriteUniqueFull walk
    ├── smoke_opcode_mix.cc      Deterministic LCG over Read/Write
    ├── ping_pong.cc             Two-tile latch hand-off writes
    ├── false_sharing.cc         Multi-tile disjoint-byte writes
    ├── memcpy.cc                Async ReadShared → WriteUniqueFull
    ├── memset.cc                Async WriteUniqueFull burst
    ├── opcode_walk.cc           Cold/hot/evict/refill with assertions
    ├── read_ex_walk.cc          Three-tile coherence walk
    └── atomic_rmw.cc            The fancy one: direct CleanUnique

ruby-book/final/chi_testbench_gem5_v2/
├── Makefile                      run-<scenario> targets
├── driver/
│   ├── rbook_testbench_gem5_v2.py  Top-level gem5 config
│   ├── cfg_rn.py                  CHI_Tile_v2 + CHI_TileCacheController_v2
│   └── address_planner.py         HNF-aware cache-line address math
└── scenarios/
    ├── idle.py
    ├── smoke_read.py
    ├── smoke_write.py
    ├── smoke_opcode_mix.py
    ├── ping_pong.py
    ├── false_sharing.py
    ├── memcpy.py
    ├── memset.py
    ├── opcode_walk.py
    ├── read_ex_walk.py
    └── atomic_rmw.py
```

## The `atomic_rmw` scenario

The `atomic_rmw` scenario is v2's answer to "a CHI transaction that's
hard to reach from CPU-style requests." Its fancy step is **a bare
`CleanUnique` emitted directly on the wire by tile 15's driver**.
Under v1's stimulus path the Ruby sequencer collapses the MemCmd
flavors that would otherwise lead to `CleanUnique` (they all become
`RubyRequestType_LD` or `_ST`), so the only way to get a `CleanUnique`
on the wire is indirectly — by doing a write on a line that was
previously acquired Shared. v2 emits it directly with no prior cache
state at all.

Shape:
```
A (tile 0)  ReadShared addr          → tile-A SC
B (tile 8)  ReadShared addr          → tile-B SC
C (tile 15) CleanUnique  addr        → SnpCleanInvalid fans out to
                                        tile-A and tile-B
C (tile 15) WriteUniqueFull addr     → writes new data via HN
A (tile 0)  ReadShared addr (re-read) → tile-A refills from HN
```

## Running

```sh
# Build
scons build/RISCV/gem5.opt -j$(nproc)

# Single scenario
make -C ruby-book/final/chi_testbench_gem5_v2 run-smoke_read RN_MODE=rnf_l2

# Full pass (rnf_l2 mode supports all 11 scenarios)
make -C ruby-book/final/chi_testbench_gem5_v2 run-all RN_MODE=rnf_l2

# RN-I mode supports the non-cache-state-dependent subset
make -C ruby-book/final/chi_testbench_gem5_v2 run-smoke_read RN_MODE=rni
make -C ruby-book/final/chi_testbench_gem5_v2 run-smoke_write RN_MODE=rni
make -C ruby-book/final/chi_testbench_gem5_v2 run-memcpy    RN_MODE=rni
make -C ruby-book/final/chi_testbench_gem5_v2 run-memset    RN_MODE=rni
```

Results from this branch (`--rn-mode=rnf_l2`, default mesh config):

| Scenario          | simTicks    |
|-------------------|-------------|
| idle              |           0 |
| smoke_read        |   1,699,000 |
| smoke_write       |     727,000 |
| smoke_opcode_mix  |   1,267,000 |
| memcpy            |  24,373,000 |
| memset            |  10,511,000 |
| ping_pong         |  10,200,000 |
| false_sharing     |  99,986,000 |
| opcode_walk       | 221,537,000 |
| read_ex_walk      |     340,000 |
| **atomic_rmw**    |     350,000 |

## Debug flags

- `ChiTestbenchGem5V2` — every driver-visible event (fiber entry, send,
  receive, CompAck, snoop response).
- `RubyCHIGeneric` — CHIGenericController's wakeup/dispatch.
- `RubyQueue` — per-MessageBuffer enqueue/dequeue (very verbose).
- `RubySlicc` — state-machine transitions inside the tile / HN-F / MN.

## What v2 gets you that v1 doesn't

- Direct CHI-opcode control: any valid CHI request can be emitted,
  including combinations that a CPU/sequencer path can't produce
  (`CleanUnique` without prior cached state, bare `ReadOnce`, specific
  `WriteNoSnp*` sizes, etc.).
- Snoop behavior is observable: `recvSnoopMsg` fires whenever the
  HN/tile back-invalidates the driver; v1's driver never sees snoops
  because they terminate at the sequencer-attached L1D.
- The send/receive path is a single Garnet hop instead of several
  layers of `Packet → RubyRequest → mandatoryQueue`, which puts
  the wire in a faulting stack trace much closer to the failure.

## What v1 still does better

- Any test that wants to exercise a CPU-level request (`Load`,
  `Store`, `AtomicLoad`, etc.) with its full LL/SC-aware sequencer
  semantics.
- Reusing RISC-V SE-mode CPU configurations directly.
- Driver code reads like a small CPU program (read/write/async_read)
  instead of a CHI-level protocol.

The two testbenches are complementary; neither obsoletes the other.
