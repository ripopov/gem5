# chi_testbench — SystemC CPU-less CHI-Garnet Mesh Testbench

A framework for driving the Chapter 17 4×4 CHI mesh with SystemC
`SC_MODULE` test sequencers instead of real RISC-V cores. Each tile
that needs stimulus is a SystemC driver whose `SC_THREAD` calls
`b_transport` or `nb_transport_fw` into the RN-F sequencer through a
per-tile `TlmToGem5Bridge64`. Loops, branches, data-dependent
conditionals, and cross-tile coordination (via `sc_event`,
`sc_semaphore`, `sc_fifo`) all work like in a standard UVM sequence
driver.

See `../../extra/CHIGarnetStandaloneChapterOutline.md` for the
architectural framing.

## Layout

| Path | Role |
|------|------|
| `driver/rbook_testbench.py`     | Top-level config: 4×4 CHI mesh + per-tile TlmToGem5Bridge + scenario dispatch |
| `driver/address_planner.py`     | HNF-aware cache-line address math |
| `scenarios/*.py`                | One Python module per scenario; exports `build(args, planner) -> [driver × 16]` |
| `analysis/` (Stage 3)           | Post-run stats parsing / scenario checks |
| `Makefile`                      | Per-scenario `run-*` targets + `run-all` |
| `src/systemc/chi_testbench/` *(out of tree)* | All C++ SystemC drivers and helpers — must live under `src/` because gem5's SConstruct only walks that tree |

## How to run

```bash
# one-time build (SystemC + TLM bridge are always included in this tree)
scons build/RISCV/gem5.opt -j$(nproc)

# run one scenario
make -C ruby-book/final/chi_testbench run-smoke_read

# run every scenario sequentially
make -C ruby-book/final/chi_testbench run-all
```

Each run creates `m5out/rbook-tb-<name>-<timestamp>/` with the usual
gem5 outputs (`config.dot`, `stats.txt`, `simout.txt`) and updates a
convenience symlink `m5out/last-<name>` pointing at the latest.

## Module architecture

All C++ components live in `src/systemc/chi_testbench/`. The Python
shell only references their auto-generated SimObject classes.

### Wiring model (per tile)

```
     SystemC domain                                 gem5 domain
┌─────────────────────────────┐   ┌────────────────────────────────┐
│ system.drivers00 (SC_MODULE)│   │ system.cpu[0] = ChiTileSlot    │
│     │ .iSocket (initiator)  │   │     └ inst_sequencer,          │
│     ▼                       │   │       data_sequencer,          │
│ system.bridges[0].tlm (tgt) │──▶│       l1i, l1d, l2             │
│  TlmToGem5Bridge64          │   │ system.bridges[0].gem5 ──▶     │
└─────────────────────────────┘   │   system.ruby._cpu_ports[0].in │
                                  │       (RN-F data sequencer)    │
                                  │   → CHI RN-F FSM → Garnet mesh │
                                  │   → HNF / SNF / MN             │
                                  └────────────────────────────────┘
```

### Component reference

| Component | Kind | Role |
|-----------|------|------|
| `ChiDriverBase`       | Abstract SystemC_ScModule | Base for every test driver. Owns the TLM initiator socket, SC_THREAD trampoline, and blocking + non-blocking transport helpers. |
| `ChiTileSlot`         | gem5 `ClockedObject` placeholder | Sits in `system.cpu[i]` so CHI_RNF can attach `inst_sequencer`, `data_sequencer`, `l1i`, `l1d`, `l2` as children with proper SimObject parentage. SystemC modules cannot hold these because their C++ base is `sc_module`, not `SimObject`. |
| `IdleDriver`          | Concrete driver | Silent no-op (`run()` returns immediately). Mount at tiles that should not generate traffic. |
| `ChiFinishBarrier`    | SystemC module | Counter primitive. Drivers call `signal_finish()` on run() return; when the Nth signal arrives, `sc_stop()` ends the simulation. Replaces ad-hoc `stop_on_finish` flags for multi-driver scenarios. |
| `ChiEventBus`         | SystemC module | Named `sc_event` registry. Two or more drivers sharing the same bus reference coordinate via `bus->wait_on(name)` / `bus->notify(name)`; events are created lazily on first lookup. |
| `SmokeReadDriver`     | Concrete driver | Sequential blocking reads over a fixed address list. |
| `SmokeWriteDriver`    | Concrete driver | Sequential blocking writes over a fixed address list. |
| `SmokeOpcodeMixDriver`| Concrete driver | Configurable read/write mix with a per-instance LCG RNG. Intended to be instantiated on every tile concurrently. |
| `PingPongDriver`      | Concrete driver | Turn-taking writes on one cache line between two tiles, synchronised via a shared `ChiEventBus`. Measures per-round latency. |
| `FalseSharingDriver`  | Concrete driver | Single-byte writes at a fixed offset inside a shared cache line. Two instances with different offsets induce the classic false-sharing invalidation storm. |
| `MemcpyDriver`        | Concrete driver | Pipelined `LD src[i]; ST dst[i]` using `async_read` / `async_write`; keeps `pipeline_depth` slots in flight. |
| `MemsetDriver`        | Concrete driver | Pipelined burst stores using `async_write`; keeps `pipeline_depth` slots in flight. |
| `OpcodeWalkDriver`    | Concrete driver | Scripted LD/ST sequence with data-dependent assertions covering cold ReadShared, WriteUnique + hot readback, capacity-forced WriteBackFull, and post-evict refill. |

### Transport modes in `ChiDriverBase`

Every concrete driver inherits both APIs and picks whichever the
scenario needs.

| API | Outstanding | TLM phase | Use when |
|-----|------------|-----------|----------|
| `read(addr, buf, len)`  | 1 per SC_THREAD | `b_transport` | Simplest; waiting for each response is the measurement. Example: ping_pong latency. |
| `write(addr, buf, len)` | 1 per SC_THREAD | `b_transport` | Same as above for stores. |
| `async_read(addr, buf, len) → Handle`   | many | `nb_transport_fw BEGIN_REQ` | Bandwidth-shaped workloads; fire K ahead, collect in order. |
| `async_write(addr, buf, len) → Handle`  | many | `nb_transport_fw BEGIN_REQ` | Same as above for stores. |
| `resolve(Handle)` / `resolve_all()`     | — | waits on `BEGIN_RESP`-fired `sc_event` | Retire a specific handle (or every in-flight handle). |

Internals of the non-blocking path:

- Payloads are allocated from a `Gem5SystemC::MemoryManager`
  (TlmToGem5Bridge's `handleBeginReq` calls `acquire()`/`release()`
  which require an MM on the payload).
- `submit_async` issues BEGIN_REQ, stores `{handle, trans, sc_event, completed=false}`.
- `nb_transport_bw` (bw callback) on BEGIN_RESP marks the handle
  `completed = true`, notifies its `sc_event`, and returns
  `TLM_COMPLETED + END_RESP` so the bridge retires the transaction.
- `resolve(h)` checks `completed` before blocking — if the response
  arrived synchronously, no lost-wakeup race.

### Python shell (`driver/rbook_testbench.py`)

1. Parses Chapter 17's Ruby/CHI/Garnet options plus `--scenario=<name>`.
2. Hardcoded defaults for the 4×4 mesh: `--num-cpus=16`,
   `--num-l3caches=16`, `--num-dirs=2`, `--topology=CustomMesh`,
   `--network=garnet`, `--chi-config=configs/example/noc_config/rbook_4x4.py`.
3. Imports `scenarios/<name>.py` and calls `build(args, planner)`,
   receiving a length-16 list of drivers (idle tiles use `IdleDriver`).
4. Instantiates 16 `ChiTileSlot` placeholders in `system.cpu` (so
   `Ruby.create_system` can wire RN-F children onto them).
5. Creates 16 `TlmToGem5Bridge64` bridges and wires
   `bridge.gem5 = ruby._cpu_ports[i].in_ports`,
   `driver.iSocket = bridge.tlm`.
6. Mounts a `SystemC_Kernel()` as a child of `Root` (not `System`,
   which would create a parent-reference cycle).

## Scenarios and results

All 8 scenarios complete end-to-end under `make run-all`. Timing
numbers are from a recent run on the default Chapter 17 topology.

| Scenario | Tiles | Transport | Exercises | Signature |
|----------|-------|-----------|-----------|-----------|
| `smoke_read`       | 1 active, 15 idle | blocking    | Address plan reaches every HNF; ReadShared path | 1 demand miss at every HNF slice |
| `smoke_write`      | 1 active, 15 idle | blocking    | ReadUnique → WriteBackFull at every HNF slice   | 1 demand miss at every HNF slice |
| `smoke_opcode_mix` | 16 concurrent     | blocking    | Full demand opcode envelope under 16-way load; shared `ChiFinishBarrier(expected=16)` | 8 misses per HNF (lines_per_tile=8); all 16 drivers log "done" before sc_stop at tick 997,001 |
| `ping_pong`        | 2 at tiles 0 & 15 | blocking + `ChiEventBus` | Cache-to-cache transfer across the mesh diagonal; measured round-trip | avg per-round write latency ≈ 108 ns (≈216 cycles at 2 GHz), matching ~4 mesh traversals + HNF pipeline |
| `false_sharing`    | 2 at tiles 0 & 15 | blocking    | SNP-vnet invalidation storm on one cache line | ~84 `SnpUnique` per L1 in each direction; 169 demand misses at HNF 7 (direct-forward path shortens others) |
| `memcpy`           | 1 at tile 7       | nb_transport, depth=4 | Pipelined LD src/ST dst under AT protocol | 256 lines HNF0→HNF15 copied in ~28.6 ms (32 KB moved) |
| `memset`           | 1 at tile 7       | nb_transport, depth=4 | Steady write-stream saturation | 256 lines across all 16 HNF stripes filled in ~12.7 ms (16 KB written) |
| `opcode_walk`      | 1 at tile 3       | blocking, with asserts | End-to-end CHI coherence invariant (cold ReadShared → WriteUnique → hot readback → capacity eviction → cold readback) | 4/4 assertions pass; panic on any byte mismatch |

### Reading the stats

Useful `stats.txt` keys (all under `systemc_kernel.system.`):

- `ruby.hnf<i>.cntrl.cache.m_demand_hits / m_demand_misses` —
  per-HNF demand accounting; confirms the address plan reaches the
  intended HNF.
- `cpu<i>.l1d.inTransLatHist.<CHIOpcode>::samples` — per-L1 snoop /
  ownership-transfer event counts (e.g. `SnpUnique` under
  false_sharing).
- `ruby.network.average_flit_latency` and per-vnet variants — network
  health metrics for bandwidth-shaped scenarios.
- `ruby.network.int_link*.flits_received` — per-link traffic;
  diagonal paths light up under ping_pong / false_sharing.

## Adding a new scenario

1. Write the C++ SC_MODULE under `src/systemc/chi_testbench/`,
   inheriting `gem5::chi_testbench::ChiDriverBase` and overriding
   `run()`. See `smoke_read.cc` for the minimal template.
2. Register the class as a Python SimObject in
   `src/systemc/chi_testbench/ChiDrivers.py` (subclass `ChiDriverBase`
   with `override_create = True`; provide its `create()` in the `.cc`).
3. List the source file and class name in
   `src/systemc/chi_testbench/SConscript`.
4. Rebuild: `scons build/RISCV/gem5.opt -j$(nproc)`.
5. Drop a Python wrapper at `scenarios/<name>.py` exporting
   `build(args, planner) -> [driver × 16]`.
6. Append `<name>` to `SCENARIOS` in the `Makefile`.

Shared primitives to reach for:

- `ChiFinishBarrier(expected=N)` when more than one driver is active
  and exactly one `sc_stop` should fire after the last participant
  finishes.
- `ChiEventBus` for cross-tile hand-off via named `sc_event`s.
- `IdleDriver` for tiles that should not generate traffic.
- `AddressPlanner.address_for_hnf(idx)` /
  `AddressPlanner.line_range(idx, n)` / `AddressPlanner.adjacent_bytes(addr, n)`
  for HNF-aware cache-line addressing.

## Stages

- **Stage 1 (done)** — Shell + base class + `ChiFinishBarrier` +
  `IdleDriver` + `ChiTileSlot`; scenarios `smoke_read`, `smoke_write`,
  `smoke_opcode_mix`.
- **Stage 2 (done)** — `ChiEventBus` for cross-tile sync;
  `nb_transport` helpers for multi-outstanding; scenarios
  `ping_pong`, `false_sharing`, `memcpy`, `memset`, `opcode_walk`.
- **Stage 3 (open)** — `analysis/parse_stats.py` +
  `analysis/check_scenario.py` + per-scenario `ValidatedRunReport.md`
  generation following the `ruby-book/final/hop_latency/` and
  `false_sharing/` conventions.
