# chi_testbench — SystemC CPU-less CHI-Garnet Mesh Testbench

A framework for driving the Chapter 17 4×4 CHI mesh with SystemC
`SC_MODULE` test sequencers instead of real RISC-V cores. Each tile
that needs stimulus is a SystemC driver whose `SC_THREAD` calls
`b_transport` / `nb_transport_fw` into the RN-F sequencer through a
per-tile `TlmToGem5Bridge64`. Loops, branches, data-dependent
conditionals, and cross-tile coordination (via `sc_event`,
`sc_semaphore`, `sc_fifo`) all work like in a standard UVM sequence
driver.

See `../../extra/CHIGarnetStandaloneChapterOutline.md` for the
architectural framing.

## Layout

| Directory | Contents |
|-----------|----------|
| `driver/`      | Python top-level config (`rbook_testbench.py`), `address_planner.py` |
| `scenarios/`   | One Python module per scenario; each exports `build(args, planner) -> [driver, ...]` returning 16 drivers (one per tile) |
| `analysis/`    | Post-run stats parsing / scenario checks (Stage 3) |
| *out of tree:* `src/systemc/chi_testbench/` | C++ SystemC drivers — must live under `src/` because gem5's SConstruct only walks that tree |

## How to run

```bash
# one-time build (SystemC + TLM bridge are always included in this tree)
scons build/RISCV/gem5.opt -j$(nproc)

# run a scenario
make -C ruby-book/final/chi_testbench run-smoke_read
```

The run target creates `m5out/rbook-tb-<name>-<timestamp>/` with the
usual gem5 outputs (`config.dot`, `stats.txt`, `simout.txt`).

## Adding a new scenario

1. Write the SC_MODULE in C++ under `src/systemc/chi_testbench/`,
   inheriting `gem5::chi_testbench::ChiDriverBase` and overriding
   `run()`. See `smoke_read.cc` for the minimal template.
2. Register the C++ class as a Python SimObject in
   `src/systemc/chi_testbench/ChiDrivers.py` (subclass `ChiDriverBase`
   with `override_create = True`; add a `create()` method in the `.cc`).
3. Add the source file in `src/systemc/chi_testbench/SConscript`.
4. Rebuild: `scons build/RISCV/gem5.opt -j$(nproc)`.
5. Add a Python scenario wrapper under
   `ruby-book/final/chi_testbench/scenarios/<name>.py` exporting
   `build(args, planner) -> [driver x 16]`.
6. Add a `run-<name>` target in the Makefile.

## Wiring model

```
     SystemC domain                                 gem5 domain
┌─────────────────────────────┐   ┌────────────────────────────────┐
│ system.drivers00 (SC_MODULE)│   │ system.cpu[0] = ChiTileSlot    │
│     │ .iSocket (initiator)  │   │     └ inst_sequencer, data_seq,│
│     ▼                       │   │       l1i, l1d, l2             │
│ system.bridges[0].tlm (tgt) │──▶│ system.bridges[0].gem5 ──▶     │
│  TlmToGem5Bridge64          │   │   system.ruby._cpu_ports[0].in │
└─────────────────────────────┘   │       (RN-F data sequencer)    │
                                  │   → CHI RN-F FSM → Garnet mesh │
                                  │   → HNF / SNF / MN             │
                                  └────────────────────────────────┘
```

`ChiTileSlot` is a tiny `ClockedObject` placeholder that sits in
`system.cpu[i]` so that `CHI_RNF.generate` can attach RubySequencer /
CHI_L1 / CHI_L2 children with proper gem5 SimObject parentage (a
SystemC module can't hold them — its stat groups collide).

## Transport modes (in `ChiDriverBase`)

- **Blocking (Stage 1)** — `read(addr, buf, len)` / `write(addr, buf, len)`.
  One outstanding per SC_THREAD. The thread suspends inside
  `b_transport` until the CHI response arrives.
- **Non-blocking (Stage 2)** — `async_read` / `async_write` / `resolve`.
  Multiple outstanding per SC_THREAD via TLM-2 AT 4-phase protocol.
  Under construction.

## Current scenarios

| Scenario | Status | What it exercises |
|----------|--------|-------------------|
| `smoke_read` | ✓ Stage 1 | One blocking read to every HNF slice; validates mesh + address plan |

## Stages

- **Stage 1 (done)** — Shell + `ChiDriverBase` with blocking API +
  `smoke_read`.
- **Stage 2** — `ping_pong`, `false_sharing`, `memcpy`, `memset`,
  `opcode_walk`; `nb_transport` helpers.
- **Stage 3** — `analysis/parse_stats.py` + `check_scenario.py` +
  per-scenario reports.
