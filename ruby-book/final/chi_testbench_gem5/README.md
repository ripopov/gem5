# chi_testbench_gem5 — CPU-less CHI/Garnet testbench (pure gem5 APIs)

Native-API counterpart to the SystemC testbench at
`ruby-book/final/chi_testbench`. Same 4×4 CHI mesh, same 8 SystemC
scenarios (plus one new `read_ex_walk`), same stimulus — but no
SystemC kernel, no TLM ⇄ Packet bridge, no `USE_SYSTEMC=true`
build flag. Each tile is a gem5 `ClockedObject` (`ChiSeqDriver`)
whose sequence runs on a `Fiber` and issues `Packet`s directly into
the RN-F sequencer's `in_ports`.

The two testbenches are byte-identical on CHI traffic (see the
**Parity** section). This testbench exists to let you read the two
implementations side by side and pick the style that fits your
project.

## Layout

| Path | Role |
|------|------|
| `driver/rbook_testbench_gem5.py` | Top-level config: 4×4 CHI mesh + scenario dispatch, no TLM bridge, no SystemC kernel |
| `driver/address_planner.py`      | HNF-aware cache-line address math (copied from the SystemC testbench) |
| `scenarios/*.py`                 | One Python module per scenario; exports `build(args, planner) -> [ChiSeqDriver × 16]` |
| `Makefile`                       | Per-scenario `run-*` targets + `run-all` |
| `src/chi_testbench_gem5/` *(out of tree)* | All C++ — ChiSeqDriver, SeqThread (Fiber subclass), sync primitives, 10 registered sequences |

## How to run

```bash
# USE_SYSTEMC is NOT required for this testbench.
scons build/RISCV/gem5.opt -j$(nproc) PROTOCOL=CHI

# one scenario
make -C ruby-book/final/chi_testbench_gem5 run-smoke_read

# all scenarios
make -C ruby-book/final/chi_testbench_gem5 run-all
```

Each run creates `m5out/rbook-tb-gem5-<scenario>-<timestamp>/` with
gem5's usual outputs and updates a convenience symlink
`m5out/last-<scenario>`.

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
│   ├─ inst_sequencer, data_sequencer, l1i, l1d, l2            │
│   │      (attached by CHI_RNF; parented under system.cpu[i]) │
│   └─ kick/wake/xfer events (EventFunctionWrapper)            │
└──────────────────────────────────────────────────────────────┘
                                 ↓ CHI RN-F FSM → Garnet mesh → HNF / SNF / MN
```

Because `ChiSeqDriver` is a `ClockedObject`, it satisfies the
sequencer's stat-namespace parent requirement directly — the
`ChiTileSlot` shim from the SystemC testbench is not needed.

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

Last-observed signatures from the committed run:

| Scenario | Drivers | Mechanism | Observed (this testbench) |
|----------|---------|-----------|---------------------------|
| `smoke_read`        | Tile 0 only | Blocking `read` | 16 HNF misses (1 per slice), sim finishes at 1.83 M ticks |
| `smoke_write`       | Tile 0 only | Blocking `write` | 16 HNF misses, 1.83 M ticks |
| `smoke_opcode_mix`  | All 16 tiles in parallel, private stripes | Blocking mixed R/W | 128 HNF misses total, 0.99 M ticks |
| `ping_pong`         | Tiles 0 & 15 alternating | Blocking `write` + `ChiEventBus` rendezvous | 100 iterations, 200 HNF misses, 21.6 M ticks |
| `false_sharing`     | Tiles 0 & 15 at different bytes of one line | Blocking `write(1 byte)` | 1000 iterations, 169 HNF misses, 16.9 M ticks |
| `memcpy`            | Tile 7 | `async_read`+`async_write` pipelined, depth 4 | 256 lines, 271 HNF misses, 28.6 M ticks |
| `memset`            | Tile 7 | `async_write` pipelined, depth 4 | 256 lines, 256 HNF misses, 12.7 M ticks |
| `opcode_walk`       | Tile 3 | Scripted LD/ST/evict walk with per-byte asserts | 4/4 assertions pass, 241.8 M ticks |
| `read_ex_walk` (new) | Tiles 0/8/15 as A/B/C | `read_exclusive` + `write` with 3-way bus rendezvous | A: 2 misses; B: 1 miss; C: 2 misses + 1 L1 hit; 2 SnpCleanInvalid fan-out; 1 CleanUnique upgrade; 0.51 M ticks |

## Parity with the SystemC testbench

After running both `chi_testbench` (SystemC) and `chi_testbench_gem5`
with identical defaults, the observable CHI metrics match exactly:

| Scenario | `simTicks` (SystemC) | `simTicks` (gem5-native) | Δ |
|----------|----------------------|--------------------------|---|
| smoke_read       | 1 833 001     | 1 833 001     | 0 |
| smoke_write      | 1 833 001     | 1 833 001     | 0 |
| smoke_opcode_mix |   997 001     |   997 001     | 0 |
| ping_pong        | 21 631 001    | 21 631 001    | 0 |
| false_sharing    | 16 867 001    | 16 867 001    | 0 |
| memcpy           | 28 643 001    | 28 643 001    | 0 |
| memset           | 12 660 001    | 12 660 001    | 0 |
| opcode_walk      | 241 751 001   | 241 751 001   | 0 |

Total HNF demand-miss counts match tick-for-tick across all 8
shared scenarios (16, 16, 128, 200, 169, 271, 256, 2049
respectively). Same network latency, same flit counts, same
per-RN-F state transitions — the two driver stacks are
indistinguishable to CHI/Garnet.

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
| `system.cpuN.l1d.outTransLatHist.Send<Op>::total` | CHI opcode each RN-F emitted |
| `system.cpuN.l1d.inTransLatHist.Snp<Op>::total`   | Snoops each RN-F received |
| `system.ruby.network.average_flit_latency`        | End-to-end flit timing |
| `system.ruby.network.int_linkN.flits_received`    | Per-link flit traffic |
| `system.simTicks`                                 | Wall-clock of the simulated run |

## Stage tracker

- [x] Stage 1 — ChiSeqDriver, SeqThread, Barrier, `idle` + 3 smoke
  sequences, Python shell + Makefile. `make run-smoke_read`
  completes with expected HNF distribution.
- [x] Stage 2 — Latch + ChiEventBus; async_read/write/resolve;
  `ping_pong`, `false_sharing`, `memcpy`, `memset`, `opcode_walk`,
  `read_ex_walk`. All 9 scenarios exit cleanly; CHI totals match
  SystemC testbench tick-for-tick.
- [x] Stage 3 — this README with the comparison table.
