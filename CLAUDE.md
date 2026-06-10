# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## gem5 Overview

gem5 is a modular computer-system architecture simulator. **This branch targets RISC-V only.** It includes detailed CPU, memory hierarchy, and device models.

## Build Commands

gem5 uses **SCons** as its build system. Build output goes to `build/<ISA>/`.



```sh
# Build optimized RISC-V gem5 (debug symbols + tracing enabled)
scons --ignore-style build/RISCV/gem5.opt -j$(nproc)

# Debug build (no optimization, full debug symbols, tracing)
scons --ignore-style build/RISCV/gem5.debug -j$(nproc)

# Fast build (max optimization, no debug/tracing, NDEBUG)
scons --ignore-style build/RISCV/gem5.fast -j$(nproc)
```

Build variants: `.debug` (-O0, debug+tracing), `.opt` (-O3, debug+tracing), `.fast` (-O3, no debug/tracing).

Requires: GCC 11+ or Clang 14+, C++17, Python 3.8+, SCons, zlib, m4.

## Running Simulations

Always pass `-d m5out/<description>-<datetime>` so each run gets its own output directory and previous results are never overwritten.
Always launch gem5 through `util/run_with_timeout.sh` instead of invoking
`build/RISCV/gem5.*` directly.
The wrapper defaults to a `5m` timeout and exists to prevent long or wedged
simulation runs from hanging agentic loops.
Pass the normal gem5 command line unchanged after the wrapper.

```sh
# Use a descriptive name + datetime stamp
./util/run_with_timeout.sh ./build/RISCV/gem5.opt -d m5out/mesi-two-level-4cpu-$(date +%Y%m%d-%H%M%S) configs/example/ruby_random_test.py ...
```

## Code Style

### C++
- 79-character line limit, 4-space indentation (no tabs)
- Upper camel case for classes (`ThisIsAClass`)
- Lower camel case for methods/member variables (`thisIsAFunction`, `memberVar`)
- Leading underscore for private members with public accessors (`_variableWithAccessor`)
- Snake case for local variables and function parameters
- ALL_CAPS for macros
- Return type on its own line, function braces on their own line
- Access modifiers (`public:`, `private:`) indented 2 spaces; members indented 4 spaces
- Formatting enforced via `.clang-format`; run `util/run-git-clang-format.py`

### Python
- Formatted with Black (79-char line limit), imports sorted with isort
- PEP 8 naming conventions
- Config in `pyproject.toml`

### Pre-commit hooks
```sh
pip install pre-commit && pre-commit install
```
Runs: trailing-whitespace, black, isort, clang-format, gem5 style checker, commit message checker.

## Commit Message Format

```
<tag(s)>: Short description (max 65 chars total)

Optional detailed description (max 72 chars per line).

Jira Issue: https://gem5.atlassian.net/browse/GEM5-XXX
```

Tags correspond to subsystem names in `MAINTAINERS.yaml` (e.g., `mem`, `cpu`, `arch-riscv`, `base`, `sim`).

## Architecture

### Core Simulation Framework

**SimObject** (`src/sim/sim_object.hh`) is the base class for all configurable hardware components. Every CPU, cache, memory controller, etc. inherits from SimObject. SimObjects are configured via Python parameter classes and instantiated by the C++ runtime.

**Initialization order**: `init()` → `regStats()` → `initState()`/`loadState()` → `resetStats()` → `startup()` → `drainResume()` (pre-order depth-first, parent before children).

**Event-driven simulation**: The simulator is a discrete event engine. `EventQueue` (`src/sim/eventq.hh`) schedules and dispatches events. The main loop is in `src/sim/simulate.cc`. Multiple event queues support parallel simulation with quantum-based synchronization.

**Port system** (`src/sim/port.hh`): SimObjects connect via RequestPort/ResponsePort pairs that carry Packet objects representing memory transactions. This is how the memory hierarchy is wired together.

### Python-C++ Integration

gem5 entry point (`src/sim/main.cc`) embeds a Python interpreter via pybind11. Simulation scripts (in `configs/`) are Python programs that configure the system by instantiating SimObject subclasses, then call `m5.simulate()` to run the C++ simulation loop.

Each SimObject has a corresponding Python class (e.g., `src/cpu/BaseCPU.py`) defining its parameters. The build system auto-generates C++ `Params` structs from these Python definitions. C++ constructors take `const <ClassName>Params &p`.

Use the `PARAMS(ClassName)` macro to simplify access to the params struct.

### Build System Registration (SCons)

Source files are registered in per-directory `SConscript` files using gem5-specific SCons functions:
- `Source('file.cc')` — register a C++ source file
- `SimObject('Object.py')` — register a SimObject Python definition (triggers codegen)
- `DebugFlag('FlagName', 'description')` — define a debug trace flag
- `GTest('test_name.cc', 'source.cc')` — register a Google Test unit test

### Kconfig Configuration System

gem5 uses a Linux kernel-style Kconfig system for build-time feature selection. Config variables (e.g., `CONF_BUILD_GPU`, `CONF_RUBY`, `CONF_USE_SYSTEMC`) are available in C++ via `#include "config/<name>.hh"` and in Python via `m5.defines.buildEnv`. See `KCONFIG.md` for details.

### Key Source Directories

- `src/arch/` — ISA implementations; each ISA has `.isa` files processed by the ISA parser (`src/arch/isa_parser/`) to generate C++ decoder/instruction classes
- `src/cpu/` — CPU models (Simple, O3 out-of-order, Minor in-order, KVM, etc.)
- `src/mem/` — Memory hierarchy: caches (`mem/cache/`), crossbars, memory controllers (DRAM, HBM, NVM), coherence protocols
- `src/mem/ruby/` — Ruby coherence protocol framework with SLICC-generated protocol implementations
- `src/sim/` — Core simulator infrastructure (SimObject, events, serialization, statistics, drain)
- `src/dev/` — Device models (storage, network, platform devices)
- `src/python/m5/` — Python runtime: parameter system (`params/`), SimObject metaclass, simulation control
- `configs/` — Example simulation configuration scripts
- `ext/` — Vendored external dependencies (pybind11, googletest, testlib, etc.)

### CHI NoC Testbench (this branch)

This branch studies the Network-on-Chip using a **CPU-less CHI 4×4 mesh testbench** (16 tiles) under `ruby-book/final/chi_testbench_gem5/`. It reuses the Chapter-17 CHI/SLICC stack verbatim and is built with `PROTOCOL=CHI` (`scons --ignore-style build/RISCV/gem5.opt PROTOCOL=CHI`).

For an detailed description of the DUT (topology diagram, tile/HNF/SNF placement, address interleaving, vnets, network parameters, traffic engine, and transaction flow), see [`ruby-book/final/chi_testbench_gem5/NocUnderTest.md`](ruby-book/final/chi_testbench_gem5/NocUnderTest.md).

Both Ruby network models are supported via `--network`: **garnet** (default — detailed flit/router model; the driver auto-sizes `--link-width-bits` for single-flit CHI data packets and sets `per_vnet_links`) and **simple** (`SimpleNetwork`, with `simple_physical_channels` for one channel per vnet). Run the same scenarios against either to compare NoC behavior.

**Per-tile wiring** (see `driver/cfg_rn.py`): there are no real CPUs. Each tile is

```
ChiSeqDriver --RequestPort--> RubySequencer -> CHI_TileCacheController -> mesh router
```

with **no L1 and no side router** — the controller connects directly to its mesh router via a single ExtLink. `ChiSeqDriver` is a `ClockedObject` that runs a Fiber-backed C++ traffic sequence. The controller is one `Base_CHI_Cache_Controller` parameterized by `--rn-mode`:
- `rnf_l2` (default): coherent L2-sized leaf cache (ReadShared/ReadUnique/snoops).
- `rni`: cache-less, DMA-like (ReadOnce/WriteNoSnp), nothing cached at the RN.

**Driver entry** (`driver/rbook_testbench_gem5.py`): builds the `System`, loads a `--scenario` module, and swaps `CHI.create_system`'s request-node/Misc-node factories via the `system._rnf_gen` / `system._mn_gen` hooks. Defaults: 16 CPUs/L3s, 2 dirs, `CustomMesh` topology, `noc_config/rbook_4x4.py`, garnet network. Key flags: `--scenario`, `--active-cores`, `--operation` (store/load), `--num-outstanding-reqs`, `--allow-retryack` (HNF retry vs. backpressure), `--rn-mode`, `--network` (garnet/simple).

**Running**: `make -C ruby-book/final/chi_testbench_gem5 run-memset|run-ping_pong|run-all` (pass `RN_MODE=rni|rnf_l2`). Each run writes a timestamped `m5out/` dir, consistent with the timeout-wrapper guidance above.

| Path | Role |
| --- | --- |
| `ruby-book/final/chi_testbench_gem5/driver/rbook_testbench_gem5.py` | Top-level config: 4×4 CHI mesh, scenario dispatch, RN-mode wiring |
| `.../driver/cfg_rn.py` | Per-tile CHI request-node wiring (`rni` / `rnf_l2`) |
| `.../driver/address_planner.py` | HNF-interleaved cache-line address math |
| `.../scenarios/{memset,ping_pong}.py` | Workload builders (one `ChiSeqDriver` per tile) |
| `src/chi_testbench_gem5/` | C++ `ChiSeqDriver`, sequences (`sequences/`), fiber sync primitives (`sync/`) |
