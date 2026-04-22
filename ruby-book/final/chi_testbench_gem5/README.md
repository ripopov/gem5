# chi_testbench_gem5 — CPU-less CHI/Garnet testbench (pure gem5 APIs)

Native-API counterpart to the SystemC testbench at
`ruby-book/final/chi_testbench`. Same 4×4 CHI mesh, same 8 SystemC
scenarios (plus one new `read_ex_walk`), same stimulus — but no
SystemC kernel, no TLM ⇄ Packet bridge, no `USE_SYSTEMC=true`
build flag. Each tile is a gem5 `ClockedObject` (`ChiSeqDriver`)
whose sequence runs on a `Fiber` and issues `Packet`s directly into
the RN-F sequencer's `in_ports`.

This testbench exists to let you read the two implementations side
by side and pick the style that fits your project. The per-tile CHI
topology is deliberately simpler than the SystemC version's
`CHI_RNF` stack: a single configurable cache controller attaches
each sequencer directly to the mesh router (no L1, no L2-in-RNF, no
side router). See [Differences from the old CHI_RNF
topology](#differences-from-the-old-chi_rnf-topology).

## Layout

| Path | Role |
|------|------|
| `driver/rbook_testbench_gem5.py` | Top-level config: 4×4 CHI mesh + scenario dispatch, no TLM bridge, no SystemC kernel |
| `driver/cfg_rn.py`               | `CHI_Tile` node + configurable `CHI_TileCacheController` (rnf_l2 / rni modes, direct-mesh attach) |
| `driver/address_planner.py`      | HNF-aware cache-line address math (copied from the SystemC testbench) |
| `scenarios/*.py`                 | One Python module per scenario; exports `build(args, planner) -> [ChiSeqDriver × 16]` |
| `Makefile`                       | Per-scenario `run-*` targets + `run-all` |
| `src/chi_testbench_gem5/` *(out of tree)* | All C++ — ChiSeqDriver, SeqThread (Fiber subclass), sync primitives, 10 registered sequences |

## How to run

```bash
# USE_SYSTEMC is NOT required for this testbench.
scons build/RISCV/gem5.opt -j$(nproc) PROTOCOL=CHI

# one scenario (default RN_MODE=rnf_l2; override with rni)
make -C ruby-book/final/chi_testbench_gem5 run-smoke_read
make -C ruby-book/final/chi_testbench_gem5 run-smoke_read RN_MODE=rni

# all scenarios
make -C ruby-book/final/chi_testbench_gem5 run-all
make -C ruby-book/final/chi_testbench_gem5 run-all RN_MODE=rni  # skips opcode_walk / read_ex_walk
```

Each run creates `m5out/rbook-tb-gem5-<scenario>-<RN_MODE>-<timestamp>/`
with gem5's usual outputs and updates a convenience symlink
`m5out/last-<scenario>-<RN_MODE>`.

## Module architecture

### Wiring (per tile)

```
                                           gem5 domain
┌──────────────────────────────────────────────────────────────┐
│ system.cpu[i] = ChiSeqDriver       (ClockedObject)           │
│   ├─ sequence="..."     (Python param)                       │
│   ├─ tile_id, addresses, iterations, ...                     │
│   ├─ SeqThread  (Fiber subclass, per-driver)                 │
│   ├─ DataPort   (RequestPort)  ──▶ system.ruby._cpu_ports[i] │
│   │                                    .in_ports             │
│   └─ kick/wake/xfer events (EventFunctionWrapper)            │
└──────────────────────────────────────────────────────────────┘
                              ↓
       system.ruby.rnf[i] = CHI_Tile
                                                       ┌─────────────────┐
          RubySequencer ── CHI_TileCacheController ──▶ │ Mesh Router[i]  │
             (in_ports)    (mode=rni | rnf_l2)         └─────────────────┘
                                                       (direct ExtLink,
                                                         no side router)
```

One sequencer, one configurable cache controller, one ExtLink to a
mesh router. No L1I, no L1D, no L2-in-RNF, no intermediate "node
router" — `CHI_Tile` is classified by `CustomMesh.distributeNodes`
(`configs/topologies/CustomMesh.py:235`) as a non-RNF node, so the
RNF-specific side-router branch is skipped.

The cache controller is parameterized at construction time by
`--rn-mode`:

- `rnf_l2` (default): coherent CHI leaf cache (L2-class size).
  Participates in ReadShared / ReadUnique / CleanUnique / SnpShared
  just like the L1D-in-RNF used to.
- `rni`: cache-less (all `alloc_on_*` disabled, 128 B dummy cache).
  Every `read`/`write` the driver issues becomes a wire transaction
  on the mesh.

Because `ChiSeqDriver` is a `ClockedObject`, it satisfies the
sequencer's stat-namespace parent requirement directly — the
`ChiTileSlot` shim from the SystemC testbench is not needed.

### RN attachment mode (`--rn-mode`)

Both modes share the `CHI_Cache_Controller` SLICC automaton; only the
allocation/coherence parameters differ. See `driver/cfg_rn.py` for the
parameter table.

| | `rnf_l2` | `rni` |
|---|---|---|
| `alloc_on_seq_acc` | True | False |
| `alloc_on_readshared/unique/writeback` | True | False |
| `allow_SD`, `send_evictions` | True | False |
| Cache | 256 KiB, 8-way | 128 B dummy, 1-way |
| Wire opcodes on a read | ReadShared / ReadUnique | ReadOnce / ReadOnceCleanInvalid |
| Wire opcodes on a write | CleanUnique + WriteBackFull / WriteUnique | WriteNoSnpFull |
| RN participates in snoops | Yes (SnpShared, SnpUnique, SnpCleanInvalid) | No |
| Supports `opcode_walk` / `read_ex_walk` | Yes | No — guard raises `m5.fatal` |

Pass the mode via `RN_MODE` to the Makefile:

```bash
make run-smoke_read                     # default RN_MODE=rnf_l2
make run-smoke_read RN_MODE=rni
make run-all RN_MODE=rni                # skip the two coherence-only scenarios
```

Output directories are keyed by mode so both can coexist:
`m5out/last-<scenario>-<rn_mode>/` (symlink).

### Component reference

| Component | Kind | Role |
|-----------|------|------|
| `ChiSeqDriver`        | `ClockedObject` (single C++ class)  | **The only driver class.** `sequence` param picks one of the registered sequences (`smoke_read`, `ping_pong`, `opcode_walk`, `read_ex_walk`, ...). Owns one `SeqThread` and one `RequestPort`. |
| `SeqThread`           | `Fiber` subclass                    | Stack-switching thread that runs the registered sequence. Each blocking call yields to gem5's main event loop; response callbacks resume the fiber. |
| `SequenceRegistry`    | C++ singleton                       | Name → `SequenceFn` table. Each `sequences/<name>.cc` registers itself at static-init time via `Registrar`. Adding a scenario means dropping one `.cc` and (optionally) one scenario Python module. |
| `Latch`               | C++ class                           | 1-to-1 notify/wait with *pending-notify* semantics (notify-before-wait is consumed on the next wait). SystemC `sc_event` analog. |
| `Barrier` (`ChiGem5Barrier`) | C++ class + `SimObject` wrapper | Counter-based completion barrier. Last `signal_finish()` calls `exitSimLoop()` to terminate. Pass by pointer via Python param. |
| `ChiEventBus` (`ChiGem5EventBus`) | `SimObject`                | Named-`Latch` registry shared between drivers. Drivers call `drv.wait_on(name)` / `drv.notify(name)`; lookups are by string and lazy. |
| `Semaphore`, `Mailbox<T>` | C++ (library)                   | Stubs for counted-resource and 1-slot FIFO rendezvous. Not used by the current 9 scenarios; available to sequence authors. |

### Sync model and the cross-fiber wake

Every sync primitive wakes fibers through the same `SeqThread::run()`
entry point, but the path differs depending on who calls whom:

- **Same-fiber wake** (primary → this driver's fiber): direct
  `seq_thread.run()` call from a gem5 event callback. This is how
  `recvTimingResp` and the scheduled wake event return control to
  the fiber after a blocking read/write or a `wait_ticks`.
- **Cross-fiber wake** (fiber A → fiber B): scheduled event. A
  direct `run()` call from inside fiber A would transfer control to
  B and leave A's stack frame dangling (the caller's function would
  never return). Instead `Latch::notify()` calls
  `waiter->drv().schedule_xfer_wake()`, which schedules a zero-delay
  event; the current fiber keeps running, and B resumes from the
  primary fiber when the event fires. This is the only piece of the
  testbench where Fiber mechanics are visible — everything else looks
  like an imperative SC_THREAD.

### Transport modes

| Mode | API | What it does |
|------|-----|-------------|
| Blocking | `drv.read(a,b,l)` `drv.write(a,b,l)` `drv.read_exclusive(a,b,l)` | Build a `Packet`, `sendTimingReq`, yield fiber; `recvTimingResp` wakes it. One outstanding per fiber. |
| Non-blocking | `drv.async_read[_exclusive]` `drv.async_write` `drv.resolve(h)` `drv.resolve_all()` | Multi-outstanding. Each call returns a `Handle`; `resolve(h)` yields until that handle's response arrives. |
| Time / rendezvous | `drv.wait_ticks(t)` `drv.wait_on(name)` `drv.notify(name)` | Schedule a wake event / latch hand-off. |

Packet construction: `MemCmd::ReadReq` for `read`, `MemCmd::WriteReq`
for `write`, `MemCmd::ReadExReq` for `read_exclusive`. **Note:** The
Ruby CHI sequencer collapses all reads to `RubyRequestType_LD` today
(`src/mem/ruby/system/Sequencer.cc` ~line 1061), so
`read_exclusive()` emits `ReadShared` on the wire — same as plain
`read()`. The API is kept forward-compatible with a Ruby protocol
that honors ReadExReq. To actually force CHI `ReadUnique` today, do
a `write()` from a tile that doesn't yet hold the line. See
`read_ex_walk` for the exclusive-ownership demo.

## Python shell (`driver/rbook_testbench_gem5.py`)

The shell is much thinner than the SystemC one:

1. Parse args (standard `common.Options` + `--scenario` + `--scenario-iterations`).
2. Resolve `scenarios/<name>.py` and call `build(args, planner)`; get a list of 16 `ChiSeqDriver` objects.
3. Build `System`, assign the drivers to `system.cpu` (they *are*
   ClockedObjects — no TileSlot indirection).
4. `Ruby.create_system(args, False, system)` attaches RN-F + HNF + SNF
   + Garnet mesh as children.
5. For each tile, wire `drv.port = system.ruby._cpu_ports[i].in_ports`.
6. `Root(full_system=False, system=system)`, `m5.instantiate()`,
   `m5.simulate()`.

No `TlmToGem5Bridge64`, no `SystemC_Kernel`, no reference-cycle
workarounds.

## Scenarios and results

Last-observed signatures (fresh runs; both modes):

| Scenario | Drivers | Mechanism | `simTicks` — `rnf_l2` | `simTicks` — `rni` |
|----------|---------|-----------|----------------------:|-------------------:|
| `smoke_read`        | Tile 0 only | Blocking `read`          |    1 509 001 |    1 501 501 |
| `smoke_write`       | Tile 0 only | Blocking `write`         |    1 509 001 |      561 501 |
| `smoke_opcode_mix`  | 16 tiles, private stripes | Blocking mixed R/W |  848 501 |      762 501 |
| `ping_pong`         | Tiles 0 & 15 alternating | Blocking `write` + `ChiEventBus` rendezvous | 14 251 501 | 7 899 001 |
| `false_sharing`     | Tiles 0 & 15 same line  | Blocking `write(1 byte)` | 14 782 501 |   72 490 001 |
| `memcpy`            | Tile 7                  | `async_read` + `async_write` pipelined | 18 759 001 | 18 471 501 |
| `memset`            | Tile 7                  | `async_write` pipelined  |    8 107 001 |    7 854 501 |
| `opcode_walk`       | Tile 3                  | Scripted LD/ST/evict walk with per-byte asserts |  196 976 501 | *guarded* |
| `read_ex_walk`      | Tiles 0 / 8 / 15 as A / B / C | `read_exclusive` + `write` with 3-way bus rendezvous | 368 001 | *guarded* |

Why the modes diverge differently per scenario:

- `false_sharing` is dramatically slower under `rni` (72 M vs 14 M
  ticks) because every single-byte write crosses the mesh to HNF.
  In `rnf_l2` the line lives in the tile's leaf cache and the two
  tiles ping-pong it via `SnpUnique` — far fewer hops per iteration.
- `smoke_write` is faster under `rni` because there is no write
  miss-fill (no cache allocation), no CleanUnique upgrade round-trip.
- `smoke_read` and `memcpy` are about the same — the first access
  always misses the HNF in either mode, and the leaf cache doesn't
  see reuse in these scenarios.

For `read_ex_walk` in `rnf_l2` the per-tile leaf cache produces the
exact CHI opcode sequence the old L1-in-RNF used to (verified by
inspecting `SendReadShared` / `SendReadUnique` / `SendCleanUnique` /
`SnpCleanInvalid` counters).

## Differences from the old CHI_RNF topology

Earlier iterations of this testbench wrapped each tile in a full
`CHI_RNF` (inst_sequencer + data_sequencer + L1I + L1D + L2 + side
router). That configuration achieved tick-for-tick parity with the
SystemC testbench at `ruby-book/final/chi_testbench` because both
implementations shared the exact same CHI stack — only the driver
mechanism differed.

The current topology intentionally drops two layers:

| Layer (old CHI_RNF) | Reason dropped |
|---|---|
| inst_sequencer + L1I | `ChiSeqDriver` never issues instruction fetches |
| L1D | Collapsed into the single tile cache |
| Intermediate "node router" (CustomMesh side router) | Only needed when an RNF has multiple controllers to bundle |

Tick parity with the SystemC testbench is therefore no longer the
goal — the two testbenches now test different CHI topologies. If you
want the old parity-matching topology back, subclass
`CHI_config.CHI_RNF` in `cfg_rn.py` instead of `CHI_RNI_DMA`.

## SystemC ↔ gem5-native: trade-offs

Both testbenches give the scenario author an imperative API with
blocking I/O, multi-outstanding requests, and cross-tile sync.
They differ in how that API is plumbed:

| Aspect | `chi_testbench` (SystemC) | `chi_testbench_gem5` (this) |
|--------|---------------------------|------------------------------|
| Build flag | Requires `USE_SYSTEMC=true` | Not required |
| Python SimObjects | 13 (one per driver + bridge, kernel, sync, slot) | 3 (ChiSeqDriver + Barrier + EventBus) |
| Config LOC (Python) | ~548 | ~470 |
| C++ LOC (framework + scenarios) | ~1187 | ~1230 |
| C++ header files | 13 | 6 |
| Per-tile wiring | TileSlot + TLM bridge + SC_MODULE driver | Just a ChiSeqDriver |
| Sync primitives | `sc_event`, `sc_semaphore`, `sc_fifo` (bundled with SystemC) | Hand-rolled `Latch`, `Barrier`, `Semaphore`, `Mailbox` (this library) |
| Fiber mechanics | Hidden inside SystemC's SC_THREAD | Visible in `SeqThread::yield_to_primary` + `schedule_xfer_wake` |
| New-scenario cost | 1 `.cc` + 1 `.hh` + Python SimObject class + scenario module | 1 `.cc` (function + `Registrar`) + scenario module |
| Typecheck / stat parenting | Needs `ChiTileSlot` shim | No shim needed (driver is a ClockedObject) |

When to pick which:

- **SystemC** if the reader already thinks in UVM / wants free
  `sc_event`, `sc_semaphore`, `sc_fifo`, or plans to integrate
  external SystemC IP into the simulation. The per-scenario C++ class
  overhead is worth the vocabulary alignment.
- **gem5-native (this)** if the project doesn't otherwise need
  SystemC, if you want one driver class across all scenarios, or if
  stat-namespace parentage matters. Adding a scenario is strictly
  less code.

## Reading stats.txt

| Metric (glob) | Question it answers |
|---------------|---------------------|
| `system.ruby.hnfN.cntrl.cache.m_demand_{hits,misses}` | Per-HNF demand pressure |
| `system.ruby.rnfN.cntrl.cache.m_demand_{hits,misses}` | Per-tile leaf-cache hits/misses (`rnf_l2`; always 0 misses under `rni`) |
| `system.ruby.rnfN.cntrl.outTransLatHist.Send<Op>::total` | CHI opcode each tile emitted |
| `system.ruby.rnfN.cntrl.inTransLatHist.Snp<Op>::total`   | Snoops each tile received (`rnf_l2` only) |
| `system.ruby.network.average_flit_latency`        | End-to-end flit timing |
| `system.ruby.network.int_linkN.flits_received`    | Per-link flit traffic |
| `system.simTicks`                                 | Wall-clock of the simulated run |

The old `system.cpuN.l1d.*` glob no longer exists — the tile cache
now lives under `system.ruby.rnfN.cntrl.*` because `ChiSeqDriver`
itself has no cache children.

## Stage tracker

- [x] Stage 1 — ChiSeqDriver, SeqThread, Barrier, `idle` + 3 smoke
  sequences, Python shell + Makefile. `make run-smoke_read`
  completes with expected HNF distribution.
- [x] Stage 2 — Latch + ChiEventBus; async_read/write/resolve;
  `ping_pong`, `false_sharing`, `memcpy`, `memset`, `opcode_walk`,
  `read_ex_walk`. All 9 scenarios exit cleanly.
- [x] Stage 3 — SystemC ↔ gem5-native comparison README.
- [x] Stage 4 — direct-mesh attachment: swap `CHI_RNF` for a thin
  `CHI_Tile` that wraps one `RubySequencer` + one configurable
  `CHI_Cache_Controller` (`driver/cfg_rn.py`). `--rn-mode={rnf_l2,rni}`
  picks leaf-cache coherence or DMA-style pass-through; neither mode
  instantiates a side router. `opcode_walk` and `read_ex_walk` guard
  against `rni` with `m5.fatal`.
