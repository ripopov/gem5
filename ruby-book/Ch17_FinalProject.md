# Chapter 17: Final Project — Build and Simulate a 4×4 CHI Mesh

> *Mastery means assembling the whole system, not just understanding the pieces.*

You have spent sixteen chapters learning gem5's memory system piece by piece.
You traced requests through the event queue, walked Classic cache miss paths, built an MSI protocol from scratch, diagnosed Garnet router pipelines, navigated CHI's SLICC decomposition, and measured DRAM scheduling effects.
This chapter asks you to put it all together: **build a complete 16-core CHI mesh system from scratch, verify it with targeted test programs, and interpret the results**.

The first two stages produce the configuration and a running system.
Stage 3 is the heart of the chapter: a series of test programs — each a small C binary running on the 16 RISC-V cores in SE mode — that progressively exercise the mesh, the coherence protocol, and the DRAM controllers.
The final stage extends `CustomMesh.py` to support per-vnet dedicated links, closing a real bandwidth fidelity gap in gem5's CHI mesh model.

## The Target System: A 4×4 CHI Mesh

Each mesh tile co-locates a RISC-V core (RN-F), a home node (HN-F), and a last-level cache slice (LLC/SLC).
Two DDR controllers (SN-F) attach at diagonally opposite corners so that memory traffic is never trivially local.

![4×4 CHI Mesh — 16 RISC-V tiles with Garnet routers, DDR controllers at opposite corners](resources/chi_mesh_4x4.svg)

The Garnet network provides cycle-accurate flit transport with XY routing across the mesh.
The CHI protocol handles coherence across all 16 LLC slices — no protocol changes required.

### Latency model

The mesh timing parameters come from RTL feedback and are configured in `rbook_4x4.py`:

| Component | Garnet parameter | Cycles | Breakdown |
|-----------|-----------------|--------|-----------|
| Mesh router | `router_latency` | 4 | 1 clk input read + 2 clk route compute + 1 clk output buffer |
| Intermediate (mux) router | `node_router_latency` | 2 | Simplified router bridging CPU request ports to the mesh |
| Inter-router link | `router_link_latency` | 4 | Repeater delay on wires between adjacent mesh routers |
| Node-to-router link | `node_link_latency` | 1 | Local connection from controller to its router (default) |

**Per-hop cost.**
Each mesh hop traverses one router and one inter-router link: 4 + 4 = **8 cycles per hop per direction**.
A round-trip request–response across *h* hops costs at least 2 × *h* × 8 = 16*h* network cycles, plus endpoint latencies (cache/protocol processing, mux router traversal).

**Example paths** (network traversal only, excluding cache/protocol overhead):

| Path | Hops | One-way (cycles) | Round-trip (cycles) |
|------|------|-------------------|---------------------|
| Core 0 → local HN-F 0 | 0 mesh hops | mux router (2) + node link (1) = 3 | 6 |
| Core 0 → HN-F 1 (adjacent) | 1 hop | 3 + 8 = 11 | 22 |
| Core 0 → HN-F 15 (diagonal) | 6 hops | 3 + 48 = 51 | 102 |

These numbers set expectations for the hop-latency test (Stage 3b): far accesses should show roughly 96 more network cycles than local accesses (6 hops × 8 cycles × 2 directions).

### Why this system

- **It exercises everything in the book**: CHI protocol (Part IV), Garnet mesh routing (Part III), Ruby controller architecture (Part II), and DRAM service (Chapter 14).
- **It is large enough to be interesting**: 16 tiles create real traffic asymmetry — a core at tile 0 requesting a line homed at tile 15 must traverse 6 router hops, while a core requesting from its local HN-F sees only the node link latency.
- **It is small enough to debug**: 4×4 keeps simulation time manageable and protocol traces readable.

## What the Reader Builds

The project proceeds in four stages.
Stages 1–2 produce the configuration files and a bootable system.
Stage 3 verifies the system with five focused test programs.
Stage 4 extends the simulator itself.

### Stage 1 — Configuration files

The reader creates two Python files.

#### 1a — The noc_config (`rbook_4x4.py`)

The reader writes `rbook_4x4.py`, extending the existing `configs/example/noc_config/2x4.py` template.
Every node class inherits from its counterpart in `configs/ruby/CHI_config.py` and overrides only the `NoC_Params` inner class (specifically `router_list`, which tells `CustomMesh.distributeNodes` which mesh router each controller attaches to).

The file defines:

- A `NoC_Params` class with `num_rows = 4` and `num_cols = 4`, giving 16 Garnet routers.
It also overrides the latency defaults with RTL-accurate values: `router_latency = 4` (1 input + 2 route + 1 output), `router_link_latency = 4` (repeater delay), and `node_router_latency = 2` (intermediate mux router).
These values flow into `CustomMesh.makeTopology` via the `setattr` loop in `CHI.py` (line 248) that copies all `NoC_Params` attributes onto the `options` namespace.
- `CHI_RNF` and `CHI_HNF` node classes with `router_list` mapping each of the 16 nodes to its corresponding router (router 0 through 15).
This is where the co-location of RN-F + HN-F + LLC at each tile happens — both classes list the same 16 routers, so `distributeNodes` attaches one RN-F and one HN-F to every mesh router.
- `CHI_SNF_MainMem` with `router_list = [0, 15]` — the two DDR controllers at diagonally opposite corners.
- `CHI_MN` — the Miscellaneous Node for DVM (Distributed Virtual Memory) operations such as TLB invalidation broadcasts.
`CHI.py` creates this node unconditionally (line 139), so every noc_config must define it.
In our RISC-V SE-mode system the MN is architecturally idle — RISC-V cores never issue ARM DVM operations — but the CHI SLICC protocol requires it to exist.

**Nodes we omit (SE-mode only).**
The CHI protocol defines three additional infrastructure node types that our system does not need.
`CHI.py` (lines 97–103) unconditionally *reads* all seven node class names from the noc_config at import time, so the classes must be defined in the file even if they are never instantiated.
Their `router_list` values do not matter — they are dead code in our scenario:

- **`CHI_SNF_BootMem`** — a memory controller for boot ROM / firmware.
Created only when `bootmem` is passed to `create_system` (line 193: `if len(other_memories) > 0`).
SE-mode simulations have no boot memory, so this node is never instantiated.
- **`CHI_RNI_DMA`** — a cacheless request node for DMA devices.
Created only when DMA ports exist (line 205: `if len(dma_ports) > 0`).
Our system has no DMA controllers.
- **`CHI_RNI_IO`** — a request node for coherent I/O agents.
Created only in full-system mode (line 214: `if full_system`).
We run in SE mode.

The reader should copy these three classes verbatim from `2x4.py` with any valid `router_list` — the values are irrelevant since the classes are never instantiated.

#### 1b — The system configuration script (`rbook_mesh_config.py`)

The reader assembles `rbook_mesh_config.py`, a Python configuration script that:

- Creates 16 RISC-V `TimingSimpleCPU` cores.
- Instantiates the CHI cache hierarchy using the legacy path (`configs/ruby/CHI.py`) with `--topology=CustomMesh` and `--chi-config=rbook_4x4.py`.
- Configures `--num-l3caches=16` so each HN-F gets an LLC slice.
- Passes `--num-dirs=2` so that two SN-F (memory) nodes are created.
- Selects the Garnet network with `--network=garnet`.
- Accepts the test binary path as a command-line argument (`--cmd`) and loads it as a shared `Process` across all 16 CPUs (SE mode requires this pattern — see `chi-with-isa.py` for reference).

The script auto-injects `--protocol=CHI` into `sys.argv` when gem5 is built with `PROTOCOL=MULTIPLE` (the default for the book's `build_opts/RISCV`), so the user never needs to pass it manually.

The existing `tests/gem5/chi_protocol/configs/chi-with-isa.py` and `configs/ruby/CHI.py` serve as reference — the reader is not writing a CHI configuration from nothing, but adapting the known patterns to a specific mesh layout.

**How DDR controllers get wired.**
The DDR connection happens in two stages, split across two files:

1. **`CHI.py`** creates two SN-F controller shells with no memory port bound (`mem_ctrl=None` at line 182).
It returns them in `mem_cntrls` to the caller.
2. **`Ruby.py`** (lines 161–202) does the actual plumbing: for each SN-F controller it creates a `MemCtrl` + `DRAMInterface` (e.g., DDR4_2400), computes cache-line-granularity address interleaving using `log2(num_dirs)` bits, binds `mem_ctrl.port` to the controller's `memory_out_port`, and sets the controller's `addr_ranges`.

With `--num-dirs=2`, the interleaving bit is bit 6 (= log₂ of the 64-byte cache line size).
Consecutive cache lines alternate between DDR0 (at router 0) and DDR1 (at router 15), spreading traffic evenly regardless of access pattern.
The reader does not write any interleaving logic — `Ruby.py` handles it automatically from `--num-dirs`.

### Stage 2 — Build and visualize

Build gem5 with the CHI protocol enabled:

```bash
scons build/RISCV/gem5.opt -j$(nproc) PROTOCOL=CHI
```

A trivial smoke-test binary (`trivial.c`) lives in `ruby-book/final/` alongside a `Makefile` that cross-compiles all test sources (see [File organization](#file-organization) below).
Build it and run the system:

```bash
make -C ruby-book/final
./build/RISCV/gem5.opt -d m5out/rbook-topology-$(date +%Y%m%d-%H%M%S) \
    configs/example/rbook_mesh_config.py \
    --cmd=ruby-book/final/trivial
dot -Tsvg m5out/rbook-topology-*/config.dot -o rbook_topology.svg
```

Open `rbook_topology.svg` and verify:
- 16 RN-F controllers, each attached to a distinct mesh router.
- 16 HN-F controllers co-located with the RN-F at the same routers.
- 2 SN-F controllers at routers 0 and 15.
- 1 MN controller.
- Garnet IntLinks forming a 4×4 mesh grid between the 16 routers.

This visual sanity check catches wiring mistakes before any test program runs.
If the topology looks wrong, fix the noc_config before proceeding.

### Stage 3 — Test programs

Each substage below is a small C program compiled for RISC-V and run under SE mode on the 16-core mesh.
The pattern follows Chapter 5b: write a focused binary, run it on the configured system, then read the statistics to confirm the expected behavior.
All test sources live in `ruby-book/final/` and are cross-compiled via the shared `Makefile`:

```bash
make -C ruby-book/final          # builds all test binaries
make -C ruby-book/final clean    # removes binaries
```

Stage-specific end-to-end targets can wrap compilation, simulation, and
analysis/report generation.
For example, `make -C ruby-book/final rbook_test_smoke` runs the full Stage 3a
workflow and writes a validated report under `ruby-book/final/smoke/`.

Run each test with the corresponding built binary path:

```bash
./build/RISCV/gem5.opt -d m5out/rbook-<name>-$(date +%Y%m%d-%H%M%S) \
    configs/example/rbook_mesh_config.py \
    --cmd=<path-to-built-test-binary>
```

For example, use `ruby-book/final/smoke/rbook_test_smoke` for Stage 3a and
`ruby-book/final/hop_latency/rbook_test_hop_latency` for Stage 3b.

#### 3a — Single-core smoke test (`smoke/rbook_test_smoke.c`)

**Goal:** verify the system boots and all 16 LLC slices are reachable from a single core.

**What it does.**
Core 0 allocates a large array (at least 16 × LLC slice size) and reads every 64th byte (one per cache line) in a sequential sweep.
With `--num-dirs=2` and `--num-l3caches=16`, address interleaving distributes cache lines across all 16 HN-F slices.
After the sweep, the program prints "PASS" and exits.

**What to check in the statistics.**
- Simulation completes without errors — the most basic validation that the mesh, protocol, and memory controllers are wired correctly.
- Per-HN-F access counts (`m_demand_hits` + `m_demand_misses` across the 16 `L3Cache_Controller` instances) are nonzero and roughly balanced.
If any HN-F shows zero accesses, the address mapping or router binding is wrong.

#### 3b — Hop-distance latency (`rbook_test_hop_latency.c`)

**Goal:** demonstrate that mesh hop count measurably affects memory access latency.

**What it does.**
Core 0 (at router 0) performs a series of cold reads.
Between each read, it flushes the L1 and L2 to force the request to travel to the HN-F.
It uses the RISC-V `rdcycle` CSR to measure the round-trip latency of each load.
The addresses are chosen so that some lines are homed at HN-F 0 (0 mesh hops — local tile) and others at HN-F 15 (6 mesh hops — diagonal corner).

The program prints the measured cycle counts for near and far accesses and exits.

**What to check in the statistics.**
- The far-HN-F reads should show measurably higher latency than near-HN-F reads.
With our RTL-calibrated latencies, each mesh hop costs `router_latency`(4) + `router_link_latency`(4) = 8 cycles per direction.
A local HN-F access (0 mesh hops) traverses only the mux router (2 cycles) and node link (1 cycle) in each direction — about 6 network cycles round-trip.
A diagonal HN-F access (6 mesh hops) adds 6 × 8 = 48 cycles per direction, for a round-trip network overhead of roughly 96 additional cycles.
The measured `rdcycle` delta between near and far reads should reflect this ~96-cycle difference, plus any protocol processing variance.
- Garnet per-link statistics should show that the far reads activate links along the full diagonal path (row 0→3, column 0→3), while near reads only touch the local ExtLink.

#### 3c — False sharing (`rbook_test_false_sharing.c`)

**Goal:** exercise the CHI invalidation protocol under two-core contention on a single cache line.

**What it does.**
Two threads, pinned to core 0 (router 0) and core 15 (router 15), repeatedly write to adjacent `int` elements in the same 64-byte cache line.
Each core performs N iterations (e.g., 10,000) of `array[my_index] += 1`.
Because both words share a cache line, every write by one core invalidates the other's copy, forcing a full CHI coherence round-trip across the mesh diagonal.
After both threads join, the program verifies the final values and prints "PASS".

**What to check in the statistics.**
- L1 cache controller stats should show a high count of invalidation-triggered misses (transitions involving `SnpUnique` or `SnpCleanInvalid`).
- The number of invalidations should be proportional to 2 × N (each core's write invalidates the other's copy).
- Garnet stats should show elevated flit traffic on the diagonal path between routers 0 and 15.
Compare with the single-core smoke test to confirm the increase comes from coherence traffic, not capacity misses.

#### 3d — Producer-consumer (`rbook_test_prodcons.c`)

**Goal:** measure coherence-mediated data handoff latency across the mesh.

**What it does.**
Core 0 (producer) writes a sequence of data values into a shared buffer, one cache line at a time.
After writing each value, it sets a per-entry flag (on a separate cache line) using a release store (`fence rw,w` + store).
Core 15 (consumer) spins on each flag using an acquire load (`load` + `fence r,rw`), then reads the corresponding data.
The consumer measures the cycle count between seeing the flag and reading the data.

After all entries are consumed, the program prints the average handoff latency and "PASS".

**What to check in the statistics.**
- The handoff latency reflects the snoop-forwarding path: consumer's load misses in L1 → request to HN-F → HN-F snoops producer's L1 → data forwarded to consumer.
This involves at least two mesh traversals (consumer→HN-F, HN-F→producer, producer→consumer), with the exact hop count depending on which HN-F owns the line.
- Protocol stats should show snoop-forwarding transitions (data supplied by a peer cache rather than by memory).
- If the producer and consumer are at opposite corners and the HN-F is near neither, the total latency includes three mesh segments — a good exercise in tracing the CHI request flow on the topology diagram.

#### 3e — Barrier synchronization (`rbook_test_barrier.c`)

**Goal:** stress-test the mesh under 16-way contention on a single shared cache line.

**What it does.**
All 16 cores execute a simple workload (e.g., sum a private array segment), then synchronize via a barrier implemented with an atomic increment (`amoadd.w`) on a shared counter.
The barrier repeats for R rounds (e.g., 100).
Each round, every core atomically increments the counter and spins until the counter reaches `16 × round`.

After all rounds complete, core 0 prints "PASS".

**What to check in the statistics.**
- The barrier counter is a single cache line that all 16 cores contend for simultaneously. This creates worst-case serialization: each atomic increment requires exclusive ownership, so 15 invalidations fan out across the mesh for every increment.
- Per-router buffer occupancy in Garnet stats should show that interior routers (which relay more paths) have higher occupancy than corner routers.
- Compare average flit latency with the single-core smoke test. The increase quantifies the cost of mesh contention.
- DRAM controller stats should show roughly balanced load between DDR0 and DDR1, confirming that the diagonal placement and interleaving work as designed even under heavy coherence traffic.

### Interpreting the results

After running all five tests, the reader has a complete picture of the system's behavior:

| Test | What it reveals | Key latency expectation |
|------|----------------|------------------------|
| Smoke | Address distribution across LLC slices, basic wiring correctness | N/A (functional check) |
| Hop latency | Mesh distance → access latency relationship, router pipeline cost | ~96-cycle round-trip delta between local and diagonal (6-hop) HN-F accesses |
| False sharing | CHI invalidation protocol cost, coherence traffic on mesh links | Each invalidation round-trip crosses 6 hops (≈102 network cycles) |
| Producer-consumer | Snoop-forwarding latency, multi-hop data transfer path | Handoff involves 2–3 mesh segments; expect ≥50 cycles per data transfer |
| Barrier | 16-way contention cost, mesh saturation, router buffer pressure | Serialized atomics amplified by multi-hop ownership transfers |

The goal is not to optimize anything — it is to **read the statistics and explain what they mean** in terms of the mesh topology, the CHI protocol, and the DRAM placement.
This is the synthesis exercise: every number in the output connects back to a mechanism the reader studied in a previous chapter.

## Failure Focus: What Goes Wrong in Configuration

Configuration-only projects have their own failure modes, distinct from protocol or RTL bugs:

- **Incorrect router bindings** — mapping two RN-F nodes to the same router but forgetting to co-locate the corresponding HN-F creates a system where coherence traffic takes unnecessary hops. The system runs, but latency is inexplicably high for some cores.
- **Missing node class definitions** — `CHI.py` unconditionally reads all seven node class names from the noc_config at import time (lines 97–103), even for node types that are never instantiated. Omitting any class — even one that is dead code in SE mode, like `CHI_SNF_BootMem` — produces an `AttributeError` before simulation begins.
- **Address interleaving mismatch** — if the two DDR controllers have overlapping or non-covering address ranges, some addresses are unmapped. Requests to those addresses produce cryptic "no match for address" errors deep inside the directory controller.
- **Wrong number of LLC slices** — setting `--num-l3caches=2` instead of 16 creates a system where all coherence traffic funnels through two HN-F nodes. The mesh topology is wasted — it becomes a de facto two-node system with 14 idle routers.

## File Organization

The project adds files in two locations:

```
configs/example/
├── noc_config/
│   └── rbook_4x4.py          # Stage 1a — 4×4 mesh noc_config
└── rbook_mesh_config.py       # Stage 1b — 16-core system configuration

ruby-book/final/
├── Makefile                   # cross-compiles all test binaries
├── smoke/
│   ├── Makefile               # Stage 3a smoke-test build/check helper
│   ├── check_smoke.py         # Stage 3a analysis
│   └── rbook_test_smoke.c     # Stage 3a
├── hop_latency/
│   └── ...                    # Stage 3b collateral and report
├── trivial.c                  # Stage 2 — minimal boot smoke test
├── rbook_test_false_sharing.c # Stage 3c
├── rbook_test_prodcons.c      # Stage 3d
└── rbook_test_barrier.c       # Stage 3e
```

The `Makefile` uses `riscv64-linux-gnu-gcc -O2 -static` and links `-lpthread` for the multi-threaded tests.
Run `make -C ruby-book/final` to build all binaries; `make -C ruby-book/final clean` to remove them.
Compiled binaries are not checked into the repository.

## Primary Code Anchors

- `configs/topologies/CustomMesh.py` — the topology that wires CHI nodes into a Garnet mesh.
- `configs/ruby/CHI_config.py` — base classes for all CHI node types (`CHI_RNF`, `CHI_HNF`, `CHI_SNF_MainMem`, etc.) and default NoC parameters.
- `configs/example/noc_config/2x4.py` — the starting template for the `rbook_4x4.py` noc_config file.
- `configs/ruby/CHI.py` — the CHI system builder that instantiates controllers and connects them to the network.
- `configs/ruby/Ruby.py` — binds DDR memory controllers to SN-F nodes with address interleaving.
- `tests/gem5/chi_protocol/configs/chi-with-isa.py` — a working CHI system on RISC-V that serves as the primary reference.
- `src/mem/ruby/protocol/chi/CHI.slicc` — the CHI protocol manifest (used as-is, not modified).
- `src/mem/ruby/network/garnet/GarnetNetwork.cc` — the Garnet network (used as-is, not modified).

## Stage 4 — Per-Vnet Dedicated Links in CustomMesh

The baseline system from Stages 1–3 has a bandwidth fidelity gap: every pair of adjacent Garnet routers is connected by a single shared link per direction, and all four CHI virtual networks (REQ, SNP, RSP, DAT) multiplex onto that one link.
Real CHI interconnects like ARM CMN use dedicated physical channels per traffic class.
gem5 already has the infrastructure to model this — `Mesh_XY.py` supports a `--per-vnet-links` flag that creates one dedicated link per vnet per direction — but `CustomMesh.py` does not.
In this stage the reader closes that gap.

### The problem

With shared links, a burst of DAT flits (64-byte cache lines) on one vnet can head-of-line block RSP flits (8-byte acknowledgements) sharing the same physical link.
This artificially couples channel latencies that would be independent on real hardware.
The `--per-vnet-links` flag in `Mesh_XY` fixes this by creating 4× the internal links (one per vnet per direction), each with `supported_vnets=[v]`.
The Garnet C++ infrastructure already handles everything: `RoutingUnit::addOutDirection` maps direction→port per vnet, `outportComputeXY` indexes by `route.vnet`, and `Topology::makeLink` filters routing table entries per vnet for TABLE routing.
Only the Python topology code in `CustomMesh._makeMesh` needs the same treatment.

### What to change

The reader modifies `configs/topologies/CustomMesh.py`:

1. **Add parameters to `_makeMesh`** — pass `per_vnet_links` (from `options`) and `num_vnets` (from `network.number_of_virtual_networks`) down from `makeTopology`.
2. **Compute the vnet list** — `vnets = list(range(num_vnets)) if per_vnet_links else [None]`, the same pattern used in `Mesh_XY.py`.
3. **Wrap each direction block in an outer `for v in vnets:` loop** — the four existing blocks (East→West, West→East, North→South, South→North) each get an outer vnet iteration.
4. **Add `supported_vnets` to each `IntLink`** — `supported_vnets=[v] if v is not None else []`. When `v` is `None` (feature off), the empty list means "all vnets" — identical to current behavior.
5. **Add `src_outport` alongside the existing `dst_inport`** — `CustomMesh` currently sets only `dst_inport` (e.g., `"West"`), not `src_outport` (e.g., `"East"`). Both are needed for XY routing to correctly map direction→port per vnet. This is harmless for TABLE routing.

The change is ~20 lines of Python mirroring what the HeteroGarnet patch already did to `Mesh_XY.py`.
Backward compatible: without `--per-vnet-links`, the loop runs once with `supported_vnets=[]`, producing identical behavior to the unmodified code.

### Verification

1. **Without the flag** — rerun the Stage 3 barrier test and confirm identical statistics. Zero behavioral change.
2. **With `--per-vnet-links`** — rerun and check Garnet's per-link `flits_per_vnet` statistics. Each internal link should carry traffic for exactly one vnet. With the flag off, links carry mixed vnet traffic.
3. **Compare latency** — the per-vnet configuration should show lower tail latency under load because DAT and RSP channels no longer contend for the same physical link.

### Code anchors

- `configs/topologies/CustomMesh.py` — `_makeMesh` (the 4 direction blocks to modify) and `makeTopology` (where `options` and `network` are available).
- `configs/topologies/Mesh_XY.py` — the reference implementation with `--per-vnet-links` already working.
- `configs/network/Network.py` — where `--per-vnet-links` is defined.
- `src/mem/ruby/network/garnet/RoutingUnit.cc` — `addOutDirection` and `outportComputeXY`, already per-vnet aware.
- `src/mem/ruby/network/garnet/Router.cc:146` — `addOutPort` passes `out_link->mVnets` to the routing unit.

## Outcome

By the end of this chapter, the reader has:

1. Written a noc_config and system configuration that maps 16 CHI nodes onto a 4×4 Garnet mesh with DDR controllers at opposite corners.
2. Visualized the topology from the generated dot graph and verified the wiring.
3. Written and run five test programs that progressively exercise the system — from single-core LLC reachability to 16-way atomic contention — and interpreted the resulting protocol, network, and DRAM statistics.
4. Extended `CustomMesh.py` to support per-vnet dedicated links, closing a bandwidth fidelity gap between gem5's CHI mesh model and real ARM CMN hardware.

The reader finishes the book having both assembled a research-grade tiled multicore simulation and improved the simulator itself — the full arc from consumer to contributor.
