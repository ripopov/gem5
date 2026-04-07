# Stage 1-2 Implementation Plan: 4x4 CHI Mesh Configuration and Boot

This document is a detailed implementation plan for Stages 1 and 2 of the final project described in `ruby-book/Ch17_FinalProject.md`.

## Overview

Stages 1-2 produce two Python files and a successful first simulation run:

- **Stage 1a** — `rbook_4x4.py`: a noc_config file that maps CHI node types onto a 4x4 Garnet mesh.
- **Stage 1b** — `rbook_mesh_config.py`: a system configuration script that creates 16 RISC-V TimingSimpleCPU cores, wires them through Ruby/CHI to the mesh, and runs an SE-mode binary.
- **Stage 2** — Build `gem5.opt` with `PROTOCOL=CHI`, run a trivial workload, and verify the topology via `config.dot`.

This document describes the architecture of each file, the call chain that connects them, and every pitfall identified in the codebase.

---

## Stage 1a: The noc_config (`rbook_4x4.py`)

### Purpose

The noc_config is consumed by `configs/ruby/CHI.py:create_system()` when `--chi-config` is passed.
`CHI.py` imports it as a raw Python module (via `importlib.machinery.SourceFileLoader`), then reads exactly these seven class names plus `NoC_Params` from it:

```
NoC_Params, CHI_RNF, CHI_HNF, CHI_MN,
CHI_SNF_MainMem, CHI_SNF_BootMem, CHI_RNI_DMA, CHI_RNI_IO
```

All eight names must exist in the file.
Missing any one causes an `AttributeError` at import time — before simulation even begins.

### Architecture

The file follows the exact pattern of `configs/example/noc_config/2x4.py`.
Every class inherits from its counterpart in `configs/ruby/CHI_config.py` and overrides only the inner `NoC_Params.router_list`.

**Top-level `NoC_Params`** — inherits `CHI_config.NoC_Params`, sets `num_rows = 4`, `num_cols = 4`.
This gives 16 Garnet mesh routers numbered 0-15 in row-major order:

```
 0 --- 1 --- 2 --- 3
 |     |     |     |
 4 --- 5 --- 6 --- 7
 |     |     |     |
 8 --- 9 ---10 ---11
 |     |     |     |
12 ---13 ---14 ---15
```

**`CHI_RNF`** — `router_list = [0, 1, 2, ..., 15]`.
16 RN-F nodes mapped one-per-router.
`distributeNodes` uses the cycling mode (`num_nodes_per_router = None` inherited from base), attaching node *i* to `router_list[i % 16]`.
Since there are exactly 16 nodes and 16 entries in `router_list`, each router gets exactly one RN-F.

**`CHI_HNF`** — `router_list = [0, 1, 2, ..., 15]`.
Same as RN-F: 16 HN-F nodes co-located with RN-F at every tile.
Co-location means both node types list the same router indices.

**`CHI_SNF_MainMem`** — `router_list = [0, 15]`.
Two DDR controllers at diagonally opposite corners.

**`CHI_MN`** — `router_list = [0]` (or any valid router).
One Misc Node for DVM; architecturally idle in RISC-V SE mode but must exist because `CHI.py` unconditionally creates it (line 139).

**Three unused node types** — `CHI_SNF_BootMem`, `CHI_RNI_DMA`, `CHI_RNI_IO`.
Copy verbatim from `2x4.py` with any valid `router_list`.
These classes are never instantiated in SE mode without DMA/bootmem, but `CHI.py` reads their names at import time (lines 97-103).

### How `distributeNodes` wires nodes to routers

`CustomMesh.distributeNodes()` has two modes controlled by `num_nodes_per_router`:

1. **`num_nodes_per_router = N`** (set explicitly) — evenly distributes N nodes per listed router.
   Asserts `len(router_list) * N == len(node_list)`.
2. **`num_nodes_per_router = None`** (default, inherited) — cycles through `router_list` round-robin for each node.

For RN-F specifically, `distributeNodes` creates an intermediate "node router" (zero-latency bridge) via `_createRNFRouter()` between the mesh router and the RN-F's controllers.
This is because an RN-F has multiple controllers (L1I, L1D, L2) that all need individual ExtLinks to the same logical router.
Non-RNF nodes (HNF, SNF, MN) attach directly to the mesh router via ExtLinks.

### Key decisions

- Use the cycling mode (leave `num_nodes_per_router = None`) for all node types.
  This is simpler and avoids the assertion trap that fires when `len(router_list) * N != len(node_list)`.
- The `router_list` for `CHI_RNF` and `CHI_HNF` must be identical (same 16 entries in same order) to achieve tile co-location.
- `NoC_Params` defaults are reasonable for a first system.
  All attributes are injected into `options` by `CHI.py` (line 246-248: `setattr(options, k, getattr(params, k))`), which then flow into `CustomMesh.makeTopology()` and network initialization.

  **Parameter-by-parameter breakdown:**

  - **`router_link_latency = 1`** — Latency (in cycles) of each IntLink between adjacent mesh routers.
    Also applied to the IntLinks connecting RN-F bridge routers to their parent mesh router (`CustomMesh._createRNFRouter()`, lines 183/193).
    This models wire propagation delay between routers.
    Read by `CustomMesh.makeTopology()` as `options.router_link_latency` (line 275) and passed to `_makeMesh()` as the `link_latency` argument (line 345).

  - **`node_link_latency = 1`** — Latency (in cycles) of each ExtLink connecting a controller (HN-F, SN-F, MN, or RN-F bridge) to its router (`CustomMesh.distributeNodes()`, lines 231/252).
    This models the wire delay from a controller to the network edge.
    Distinct from `router_link_latency` so that controller-to-router hops can be tuned independently of router-to-router hops.

  - **`router_latency = 1`** — Pipeline depth (in cycles) of each Garnet mesh router.
    Applied as the `latency` parameter when creating `Router` objects (`CustomMesh.makeTopology()`, line 335).
    Controls how many cycles a flit spends traversing the router's internal pipeline (route compute, VC allocation, switch allocation, crossbar traversal).
    With `latency=1`, all pipeline stages complete in one cycle.
    Note: RN-F bridge routers created by `_createRNFRouter()` use a separate hardcoded latency of 1 for Garnet (line 273: `self.node_router_latency = 1 if options.network == "garnet" else 0`), not this parameter.

  - **`router_buffer_size = 4`** — **Only used with SimpleNetwork**, not Garnet.
    Applied at `CHI.py` line 242: `if options.network == "simple": ruby_system.network.buffer_size = params.router_buffer_size`.
    For Garnet, input buffer depth is controlled by `--vcs-per-vnet` (default 4, set in `Network.py` line 92) — each VC has a buffer that can hold one flit.
    This parameter is effectively dead code in our Garnet-based system.

  - **`data_width = 32`** — Size in bytes of the data channel.
    Applied in two places by `CHI.py`:
    (1) `cntrl.data_channel_size = params.data_width` (line 233) — sets the data payload width for all CHI controllers.
    (2) `ruby_system.network.data_msg_size = params.data_width` (line 240) — tells the network how many bytes a DAT flit carries.
    A 64-byte cache line with `data_width=32` requires ceil(64/32) = 2 data flits per transfer.
    This directly affects DAT channel bandwidth: each link carries one 32-byte flit per cycle.
    Note: the Garnet NI flit size is set separately from `--link-width-bits` (default 128 bits = 16 bytes, `Network.py` line 181).
    The NI serializes messages into flit-sized chunks, so a 32-byte DAT message becomes ceil(32/16) = 2 Garnet flits on the wire.

  - **`cntrl_msg_size = 8`** — Size in bytes of control messages (REQ, SNP, RSP channels).
    Applied at `CHI.py` line 239: `ruby_system.network.control_msg_size = params.cntrl_msg_size`.
    These are small messages (addresses, command codes, no data payload) that fit in a single Garnet flit (8 bytes < 16-byte flit size), so they traverse each link in one cycle.

---

## Stage 1b: The system configuration script (`rbook_mesh_config.py`)

### Purpose

This is the top-level script passed to `gem5.opt`.
It creates the full simulated system: CPUs, Ruby/CHI cache hierarchy, Garnet network, DDR controllers, and SE-mode workload.

### Architecture

The script follows the pattern of `configs/deprecated/example/se.py` combined with the Ruby setup from `configs/example/ruby_random_test.py`.
It uses the **legacy API** (not the stdlib `SimpleBoard` path).

#### Why the legacy API, not stdlib SimpleBoard

The stdlib CHI cache hierarchies (`PrivateL1CacheHierarchy`, `PrivateL1PrivateL2CacheHierarchy` in `src/python/gem5/components/cachehierarchies/chi/`) are hardcoded to use `SimplePt2Pt` — a fully-connected point-to-point network with no Garnet modeling, no mesh structure, and no router/arbitration detail.
There is no stdlib component that supports:

- **CustomMesh topology** — the only topology that reads a noc_config file and maps CHI node types to specific mesh routers.
- **Garnet network** — the cycle-accurate flit-level network model needed to observe router pipeline latency, per-link utilization, and mesh hop effects.
- **Custom noc_config files** — the `--chi-config` mechanism exists only in the legacy `configs/ruby/CHI.py` path.

The `chi-with-isa.py` reference (used in gem5's CI tests) demonstrates the stdlib path but uses `SimplePt2Pt`, which collapses the entire network to a flat crossbar — exactly what we do NOT want.
Our project's core goal is to observe how mesh topology, hop distance, and link contention affect latency and throughput.
That requires Garnet + CustomMesh, which is only available through the legacy config path.

#### Call chain overview

```
rbook_mesh_config.py
  |
  +--> Options.addCommonOptions(parser)      # standard CLI flags
  +--> Options.addSEOptions(parser)          # --cmd, --options
  +--> Ruby.define_options(parser)           # --topology, --network, --chi-config, etc.
  |
  +--> System(cpu=[...], mem_ranges=[...])   # create system shell
  +--> Process(...)                           # SE workload
  +--> Ruby.create_system(args, False, system)
  |      |
  |      +--> Network.create_network()        # GarnetNetwork + link/router classes
  |      +--> CHI.create_system()             # create all CHI nodes
  |      |      |
  |      |      +--> read_config_file()       # import rbook_4x4.py
  |      |      +--> CHI_RNF.generate()       # 16 RN-F with L1+L2
  |      |      +--> CHI_MN.generate()        # 1 MN
  |      |      +--> CHI_HNF(...)             # 16 HN-F with LLC slices
  |      |      +--> CHI_SNF_MainMem(...)     # 2 SN-F (no mem_ctrl yet)
  |      |      +--> create_topology()        # CustomMesh.makeTopology()
  |      |
  |      +--> topology.makeTopology()         # mesh IntLinks + ExtLinks
  |      +--> Network.init_network()          # Garnet bridges, NIs
  |      +--> setup_memory_controllers()      # DDR + interleaving
  |
  +--> connectCpuPorts / createInterruptController
  +--> m5.instantiate() -> m5.simulate()
```

#### Key sections of the script

**1. Argument parsing and defaults.**
Must call `Options.addCommonOptions(parser)`, `Options.addSEOptions(parser)`, and `Ruby.define_options(parser)`.
The Ruby options pull in `Network.define_options()` which adds `--topology`, `--network`, `--per-vnet-links`, etc.
`CHI.define_options()` adds `--chi-config` and `--enable-dvm`.

The script must either hardcode or pass via CLI:
- `--num-cpus=16`
- `--num-l3caches=16`
- `--num-dirs=2`
- `--topology=CustomMesh`
- `--network=garnet`
- `--chi-config=<path-to-rbook_4x4.py>`
- `--cmd=<path-to-test-binary>`

**2. CPU creation.**
Create 16 `TimingSimpleCPU` objects:
```
system = System(
    cpu=[TimingSimpleCPU(cpu_id=i) for i in range(16)],
    mem_mode="timing",
    mem_ranges=[AddrRange(mem_size)],
    cache_line_size=64,
)
```

**3. Workload assignment.**
Create one `Process` object and assign it to all 16 CPUs:
```
for cpu in system.cpu:
    cpu.workload = process
    cpu.createThreads()
system.workload = SEWorkload.init_compatible(binary_path)
```

In SE mode with pthreads, all CPUs share the same address space via a single `Process`.
The pthread library creates threads that the OS scheduler maps to CPUs.
Each CPU still needs `createThreads()` called to initialize its thread contexts.

**4. Ruby system creation.**
`Ruby.create_system(args, False, system)` is the single call that:
- Creates the Garnet network.
- Calls `CHI.create_system()` which imports the noc_config, creates all nodes, and builds the topology.
- Calls `setup_memory_controllers()` which creates `MemCtrl` + `DRAMInterface` objects and interleaves them across the SN-F controllers.

**5. CPU-to-Ruby wiring.**
After `Ruby.create_system()`, the script must:
```
for i in range(16):
    system.cpu[i].createInterruptController()
    system.ruby._cpu_ports[i].connectCpuPorts(system.cpu[i])
```

`_cpu_ports` is a list of `CPUSequencerWrapper` objects created by `CHI_RNF`.
Each wrapper holds an instruction sequencer and a data sequencer.
`connectCpuPorts()` wires `cpu.icache_port` to the instruction sequencer and all cached data ports to the data sequencer.

**6. Clock domain.**
Use a single 2 GHz clock domain for the entire system (CPUs, Ruby, Garnet):
```
system.clk_domain = SrcClockDomain(clock="2GHz", voltage_domain=VoltageDomain())
```

Production configs often use separate clock domains for CPUs and the NoC (e.g., CPU at 3 GHz, mesh at 2 GHz).
For this project we intentionally use one shared domain because:
- **Cycle counts are directly comparable** — "10 cycles" means the same thing whether it's a router pipeline stage or a cache tag lookup. No need to mentally convert between clock domains when reading statistics.
- **Simpler config** — fewer objects, fewer parameters, fewer things to get wrong.
- **Matches the learning gem5 pattern** — `configs/learning_gem5/part3/simple_ruby.py` uses a single domain for the same reason.

A reader who wants to explore the effect of frequency asymmetry can add separate `cpu_clk_domain` and `ruby.clk_domain` later — that's a one-line change, not an architectural decision.

**7. Instantiate and run.**
```
root = Root(full_system=False, system=system)
m5.instantiate()
exit_event = m5.simulate()
```

### How DDR controllers get wired (detail)

This is split across two files and happens automatically — the reader writes zero interleaving code:

1. `CHI.py` (line 181-184) creates `CHI_SNF_MainMem(ruby_system, None, None)` for each of `--num-dirs` controllers.
   The third argument `mem_ctrl=None` means the SN-F is created as a shell with no memory port bound.

2. `Ruby.py:setup_memory_controllers()` (line 134-208) iterates over the SN-F controllers (called `dir_cntrls` in its API).
   For each SN-F it:
   - Creates a `MemCtrl` + `DRAMInterface` (type from `--mem-type`, default `DDR3_1600_8x8`).
   - Computes interleaving: `intlv_size = cacheline_size = 64`, `intlv_bits = log2(num_dirs) = 1`.
   - Sets the DRAM range with `intlvBits=1, intlvMatch=i` so consecutive 64-byte lines alternate between DDR0 (match=0) and DDR1 (match=1).
   - Binds `mem_ctrl.port = dir_cntrl.memory_out_port`.

### How HNF address interleaving works (detail)

`CHI_HNF.createAddrRanges()` (CHI_config.py line 636-652) sets up interleaved ranges across all 16 HNFs:
- `llc_bits = log2(16) = 4` — 4 bits of interleaving.
- `numa_bit = block_size_bits + llc_bits - 1 = 6 + 4 - 1 = 9`.
- Each HNF gets `intlvHighBit=9, intlvBits=4, intlvMatch=i`.
- This means bits [9:6] of the address select which of the 16 HNFs owns each cache line.
- The LLC cache's `start_index_bit = intlvHighBit + 1 = 10`, so the cache indexes above the interleaving bits.

---

## Stage 2: Build, Run, and Visualize

### Build

```bash
scons build/RISCV/gem5.opt -j$(nproc)
```

The `build_opts/RISCV` file has been configured with `PROTOCOL="MULTIPLE"` and `RUBY_PROTOCOL_CHI=y` (alongside MI_example and MSI), so a plain build includes CHI support.
No `PROTOCOL=CHI` override is needed on the command line.
The CHI SLICC protocol makes `CHI_Cache_Controller`, `CHI_Memory_Controller`, `CHI_MiscNode_Controller` available as SimObjects.
If CHI were missing from the build, the config script would fail with import errors at startup.

### Trivial test binary

For Stage 2, the reader needs only a minimal binary that exits immediately, to verify the system boots.
A simple C program that prints a message and returns 0, cross-compiled with:
```bash
riscv64-linux-gnu-gcc -O2 -static -o trivial trivial.c
```

The `-static` flag is essential for SE mode — gem5 SE does not support dynamic linking / shared libraries.

### Run and dump topology

```bash
./build/RISCV/gem5.opt -d m5out/rbook-topology-$(date +%Y%m%d-%H%M%S) \
    rbook_mesh_config.py --cmd=trivial
```

gem5 writes `config.dot` to the output directory by default (controlled by `--dot-config` flag in gem5's main.py).

```bash
dot -Tsvg m5out/rbook-topology-*/config.dot -o rbook_topology.svg
```

### Verification checklist

In the generated topology graph, verify:
- 16 RN-F controllers, each at a distinct mesh router (routers 0-15).
- 16 HN-F controllers co-located at the same 16 routers.
- 2 SN-F controllers at routers 0 and 15.
- 1 MN controller.
- IntLinks forming a 4x4 mesh grid (24 bidirectional pairs = 48 unidirectional links).
- ExtLinks connecting controllers to their respective routers.

---

## Pitfalls and Failure Modes

### P1: Missing node class definitions → AttributeError

**What:** `CHI.py` lines 97-103 unconditionally read all seven node class names from the imported noc_config:
```python
CHI_RNF = chi_defs.CHI_RNF
CHI_HNF = chi_defs.CHI_HNF
CHI_MN = chi_defs.CHI_MN
CHI_SNF_MainMem = chi_defs.CHI_SNF_MainMem
CHI_SNF_BootMem = chi_defs.CHI_SNF_BootMem
CHI_RNI_DMA = chi_defs.CHI_RNI_DMA
CHI_RNI_IO = chi_defs.CHI_RNI_IO
```

**Risk:** Omitting any of the three "unused" classes (BootMem, DMA, IO) causes an immediate `AttributeError`.

**Mitigation:** Always define all seven classes, even if three are dead code.
Copy unused ones verbatim from `2x4.py`.

### P2: `num_cpus` vs `num_l3caches` mismatch

**What:** `CHI.py` line 123 asserts `len(cpus) == options.num_cpus`.
The script must set `--num-cpus=16`.
Separately, `--num-l3caches=16` controls how many HN-F nodes are created (line 163-168).

**Risk:** If `--num-l3caches` differs from 16, the system still runs but:
- Fewer HN-F nodes means some routers have no HN-F, wasting mesh locality.
- `num_l3caches` must be a power of 2 because `CHI_HNF.createAddrRanges` computes `llc_bits = int(math.log(len(hnfs), 2))`.
  A non-power-of-2 value silently truncates the log and produces incorrect interleaving.

**Mitigation:** Always set `--num-l3caches=16` for a 4x4 mesh.

### P3: `num_dirs` must be a power of 2

**What:** `Ruby.py:setup_memory_controllers()` computes `intlv_bits = int(math.log(options.num_dirs, 2))`.

**Risk:** Non-power-of-2 values (e.g., 3) silently produce truncated interleaving bits, causing address range gaps.

**Mitigation:** Use `--num-dirs=2`.

### P4: `router_list` length vs node count

**What:** In cycling mode (`num_nodes_per_router = None`), `distributeNodes` maps node *i* to `router_list[i % len(router_list)]`.
If `router_list` has fewer entries than nodes, multiple nodes share a router.
If it has more, some routers get no nodes of that type.

**Risk:** For CHI_RNF with 16 CPUs, a `router_list` with only 8 entries maps two RN-F per router to 8 routers, leaving 8 routers with no CPU — a valid but likely unintended topology.

**Mitigation:** `router_list` for CHI_RNF and CHI_HNF must have exactly 16 entries matching the 16 routers.
For CHI_SNF_MainMem, exactly 2 entries matching `--num-dirs=2`.

### P5: `--topology=CustomMesh` requires `--chi-config`

**What:** `CHI.py` line 88-89: if `options.topology == "CustomMesh"` and `options.chi_config` is None, gem5 calls `m5.fatal()`.

**Risk:** Forgetting `--chi-config` produces a cryptic error about missing noc-config.

**Mitigation:** Always pass `--chi-config=<path>` with `--topology=CustomMesh`.

### P6: `NoC_Params` propagation into `options`

**What:** `CHI.py` lines 246-248 copy all attributes from the noc_config's `NoC_Params` class into `options` via `setattr`.
`CustomMesh.makeTopology()` then reads `options.num_rows`, `options.num_cols`, `options.router_link_latency`, `options.node_link_latency`, `options.cross_links`, `options.cross_link_latency`.

**Risk:** If the top-level `NoC_Params` in the noc_config doesn't define `num_rows`/`num_cols`, these attributes are inherited from `CHI_config.NoC_Params` which doesn't define them either — causing `AttributeError` in `CustomMesh`.

**Mitigation:** The noc_config's `NoC_Params` must explicitly set `num_rows = 4` and `num_cols = 4`.

### P7: Routing algorithm for CustomMesh

**What:** `Network.py` defines `--routing-algorithm` (default 0 = weight-based TABLE).
`CustomMesh._makeMesh()` sets `dst_inport` on IntLinks (e.g., `"West"`, `"South"`) but does NOT set `src_outport`.
XY routing (`--routing-algorithm=1`) requires both `src_outport` and `dst_inport` to correctly map direction→port in `RoutingUnit::outportComputeXY`.

**Risk:** Using `--routing-algorithm=1` (XY) with the unmodified `CustomMesh` may produce incorrect routing because `src_outport` is missing.
The default TABLE routing (algorithm 0) works correctly because it uses link weights, not port direction names.

**Mitigation:** Use the default routing algorithm (0 = TABLE) for Stages 1-2.
TABLE routing uses the `weight` parameter on IntLinks (East/West=1, North/South=2) to enforce XY-like deadlock-free routing via shortest paths.
Stage 4 of the chapter adds `src_outport` to enable true XY routing.

### P8: `config.dot` may be very large for 16-node systems

**What:** The dot graph includes all SimObjects and their port connections.
A 16-core CHI system has hundreds of SimObjects (16 L1I + 16 L1D + 16 L2 + 16 HNF + 2 SNF + 1 MN + 16 routers + bridge routers + network interfaces).

**Risk:** `config.dot` may produce an unreadable SVG. GraphViz can also be slow or run out of memory for very large graphs.

**Mitigation:** The purpose of the dot visualization is a rough sanity check, not a detailed diagram.
Use `dot -Tsvg` (not `-Tpdf`) for faster rendering.
Alternatively, inspect the topology programmatically by reading `config.ini` or `config.json` from the output directory.

### P9: SE-mode workload wiring

**What:** In SE mode, each CPU's workload is a `Process` object.
For multi-threaded binaries using pthreads, a single `Process` must be shared across all CPUs.
Each CPU needs `createThreads()` called to initialize thread contexts.
The system also needs `system.workload = SEWorkload.init_compatible(binary_path)`.

**Risk:** Creating separate `Process` objects per CPU creates 16 independent address spaces — threads won't see each other's memory, and pthreads synchronization breaks silently.

**Mitigation:** Create exactly one `Process` and assign it to all 16 CPUs.

### P10: `--mem-size` must be large enough

**What:** `Ruby.py:setup_memory_controllers()` creates DRAM with ranges based on `--mem-size` (default "512MiB").

**Risk:** If the test binary allocates more memory than `--mem-size`, the simulation hits "no match for address" errors inside the directory controller.
With 16 cores, aggregate working set can be larger than expected.

**Mitigation:** Use at least 512MiB (the default). For the Stage 3 test programs with 16-core workloads, this is sufficient.

### P11: `noc_config` import mechanism

**What:** `CHI.py:read_config_file()` uses `importlib.machinery.SourceFileLoader` with a hardcoded module name `"chi_configs"`.
The noc_config file must contain `from ruby import CHI_config` at the top.

**Risk:** If the noc_config is not on the Python path, the `from ruby import CHI_config` import inside it fails.
gem5 adds `configs/` to `sys.path`, so `ruby.CHI_config` resolves correctly as long as gem5 is run from the repo root or `configs/` is in the path.

**Mitigation:** Place `rbook_4x4.py` in `configs/example/noc_config/` (alongside `2x4.py`), or ensure the script is run from the gem5 root directory.
The `--chi-config` path should be absolute or relative to the CWD, not relative to the config script.

### P12: `CustomMesh` does not implement `registerTopology`

**What:** `Ruby.py` line 271 calls `topology.registerTopology(options)` in SE mode.
`CustomMesh` inherits the no-op default from `BaseTopology`, which does nothing.
The `Mesh_XY` topology implements it to register CPU-to-memory mappings with the faux filesystem.

**Risk:** No functional impact — the faux filesystem entries are optional and only used by some OS-aware workloads.
But if a future test relies on `/proc/cpuinfo` or `/sys/` entries in SE mode, they won't be populated.

**Mitigation:** Not a problem for Stages 1-2. Can be noted as a limitation.

### P13: The `_cpu_ports` ordering must match CPU ordering

**What:** `CHI.py` builds `cpu_sequencers` by iterating over `ruby_system.rnf` in order.
`CHI_RNF.generate()` creates one RNF per CPU in the `cpus` list order.
The system script connects `system.ruby._cpu_ports[i]` to `system.cpu[i]`.

**Risk:** If the CPU list passed to `Ruby.create_system()` is in a different order than `system.cpu`, port *i* connects to the wrong CPU.
The default behavior of `Ruby.create_system()` uses `system.cpu` when `cpus` is not explicitly passed (Ruby.py line 248-249).

**Mitigation:** Either don't pass `cpus` (let it default to `system.cpu`), or pass `system.cpu` explicitly.
The default is safe as long as `system.cpu` is the list of all CPUs.

---

## File Placement

| File | Recommended location |
|------|---------------------|
| `rbook_4x4.py` | `configs/example/noc_config/rbook_4x4.py` |
| `rbook_mesh_config.py` | `configs/example/rbook_mesh_config.py` |
| `trivial.c` (Stage 2 smoke test) | `ruby-book/final/trivial.c` |

The noc_config goes alongside `2x4.py` so the `from ruby import CHI_config` import works without path manipulation.
The system config goes in `configs/example/` following gem5 convention, using `addToPath("../")` to access `common` and `ruby` modules.

---

## Summary of CLI invocation

```bash
# Build (CHI included via build_opts/RISCV PROTOCOL="MULTIPLE")
scons build/RISCV/gem5.opt -j$(nproc)

# Run (Stage 2)
./build/RISCV/gem5.opt -d m5out/rbook-topology-$(date +%Y%m%d-%H%M%S) \
    configs/example/rbook_mesh_config.py \
    --num-cpus=16 \
    --num-l3caches=16 \
    --num-dirs=2 \
    --network=garnet \
    --topology=CustomMesh \
    --chi-config=configs/example/noc_config/rbook_4x4.py \
    --cmd=ruby-book/final/trivial

# Visualize
dot -Tsvg m5out/rbook-topology-*/config.dot -o rbook_topology.svg
```

Note: some of these flags can be hardcoded in the script (e.g., `--num-cpus`, `--num-l3caches`, `--num-dirs`, `--network`, `--topology`, `--chi-config`) to reduce the command-line burden.
The `--cmd` should remain a CLI argument so different test binaries can be swapped in Stage 3.
