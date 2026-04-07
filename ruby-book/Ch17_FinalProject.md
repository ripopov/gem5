# Chapter 17: Final Project — Build and Simulate a 4×4 CHI Mesh

> *Mastery means assembling the whole system, not just understanding the pieces.*

You have spent sixteen chapters learning gem5's memory system piece by piece.
You traced requests through the event queue, walked Classic cache miss paths, built an MSI protocol from scratch, diagnosed Garnet router pipelines, navigated CHI's SLICC decomposition, and measured DRAM scheduling effects.
This chapter asks you to put it all together: **build a complete 16-core CHI mesh system from scratch, boot a multicore test on it, and interpret the results**.

The first four stages use the stock CHI SLICC protocol and Garnet routers unmodified — the challenge is entirely in the configuration: wiring 16 RISC-V cores, 16 private L1 caches, 16 home nodes with LLC slices, and 2 DDR controllers into a coherent mesh that actually runs.
The final stage takes one step further: extending `CustomMesh.py` to support per-vnet dedicated links, closing a real bandwidth fidelity gap in gem5's CHI mesh model.

## The Target System: A 4×4 CHI Mesh

Each mesh tile co-locates a RISC-V core (RN-F), a home node (HN-F), and a last-level cache slice (LLC/SLC).
Two DDR controllers (SN-F) attach at diagonally opposite corners so that memory traffic is never trivially local.

```
                         4×4 CHI Mesh

         DDR0──┐
               |
    ┌──────────┼──────────┬──────────┬──────────┐
    │ C0+HN0+L0│ C1+HN1+L1│ C2+HN2+L2│ C3+HN3+L3│
    ├──────────┼──────────┼──────────┼──────────┤
    │ C4+HN4+L4│ C5+HN5+L5│ C6+HN6+L6│ C7+HN7+L7│
    ├──────────┼──────────┼──────────┼──────────┤
    │ C8+HN8+L8│ C9+HN9+L9│C10+HN10+L10│C11+HN11+L11│
    ├──────────┼──────────┼──────────┼──────────┤
    │C12+HN12+L12│C13+HN13+L13│C14+HN14+L14│C15+HN15+L15│
    └──────────┴──────────┴──────────┼──────────┘
                                     │
                                     └──DDR1
```

The Garnet network provides cycle-accurate flit transport with XY routing across the mesh.
The CHI protocol handles coherence across all 16 LLC slices — no protocol changes required.

### Why this system

- **It exercises everything in the book**: CHI protocol (Part IV), Garnet mesh routing (Part III), Ruby controller architecture (Part II), and DRAM service (Chapter 14).
- **It is large enough to be interesting**: 16 tiles create real traffic asymmetry — a core at tile 0 requesting a line homed at tile 15 must traverse 6 router hops, while a core requesting from its local HN-F sees only the node link latency.
- **It is small enough to debug**: 4×4 keeps simulation time manageable and protocol traces readable.

## What the Reader Builds

The project proceeds in four stages, each producing a runnable artifact.

### Stage 1 — The noc_config file

The reader writes a `4x4.py` noc_config file, extending the existing `configs/example/noc_config/2x4.py` template.
This file defines:

- A `NoC_Params` class with `num_rows = 4` and `num_cols = 4`, giving 16 Garnet routers.
- `CHI_RNF` and `CHI_HNF` node classes with `router_list` mapping each of the 16 nodes to its corresponding router (router 0 through 15).
- `CHI_SNF_MainMem` with `router_list = [0, 15]` — the two DDR controllers at opposite corners.
- `CHI_SNF_BootMem`, `CHI_RNI_DMA`, `CHI_RNI_IO`, and `CHI_MN` bindings for the remaining infrastructure nodes.

Every node class inherits from its counterpart in `configs/ruby/CHI_config.py` and overrides only the `NoC_Params` inner class.
The reader must understand how `CustomMesh.distributeNodes` uses these router lists to attach controllers to the mesh — this is where the co-location of RN-F + HN-F + LLC at each tile happens.

### Stage 2 — The system configuration script

The reader assembles a Python configuration script that:

- Creates 16 RISC-V `TimingSimpleCPU` cores (or `MinorCPU` for more realistic timing).
- Instantiates the CHI cache hierarchy using the legacy path (`configs/ruby/CHI.py`) with `--topology=CustomMesh` and `--chi-config=<path-to-4x4.py>`.
- Configures `--num-l3caches=16` so each HN-F gets an LLC slice.
- Attaches two `DDR4_2400_16x64` memory channels, one per SN-F, with address interleaving across them.
- Selects the Garnet network with `--network=garnet`.

The existing `tests/gem5/chi_protocol/configs/chi-with-isa.py` and `configs/ruby/CHI.py` serve as reference — the reader is not writing a CHI configuration from nothing, but adapting the known patterns to a specific mesh layout.

### Stage 3 — Build and boot

Build gem5 with the CHI protocol enabled and run the configuration:

- Build: `scons build/RISCV/gem5.opt -j$(nproc)` (with `PROTOCOL=CHI` in the build options).
- First smoke test: run with a `LinearGenerator` traffic source instead of real cores to verify the mesh, protocol, and memory controllers are wired correctly. If this deadlocks or crashes, the problem is in the configuration, not in application code.
- Boot test: run `ruby_random_test.py` or a simple multithreaded RISC-V binary across all 16 cores. Confirm that the simulation completes without protocol errors or assertion failures.

### Stage 4 — Observe and interpret

With the system running, the reader collects and interprets:

- **Ruby protocol statistics** — per-controller hit/miss rates, transition counts, and average latency breakdowns. Do the 16 LLC slices share load roughly evenly, or does address hashing create hotspots?
- **Garnet network statistics** — average flit latency, per-link utilization, router buffer occupancy. Do the corner routers (near DDR0 and DDR1) show higher utilization than interior routers?
- **DRAM controller statistics** — row-buffer hit rates, bank conflict rates, queue occupancy at each DDR controller. Does the diagonal placement create balanced memory traffic, or does one controller see significantly more load?
- **End-to-end latency distribution** — what is the difference in observed memory latency between a core adjacent to a DDR controller (tile 0) and a core maximally far from both (tile 5 or 10)?

The goal is not to optimize anything — it is to **read the statistics and explain what they mean** in terms of the mesh topology, the CHI protocol, and the DRAM placement.
This is the synthesis exercise: every number in the output connects back to a mechanism the reader studied in a previous chapter.

## Failure Focus: What Goes Wrong in Configuration

Configuration-only projects have their own failure modes, distinct from protocol or RTL bugs:

- **Incorrect router bindings** — mapping two RN-F nodes to the same router but forgetting to co-locate the corresponding HN-F creates a system where coherence traffic takes unnecessary hops. The system runs, but latency is inexplicably high for some cores.
- **Missing node types** — forgetting `CHI_MN` (the miscellaneous node for DVM) or `CHI_SNF_BootMem` causes simulation crashes or hangs during boot, with error messages that point to Ruby internals rather than the missing configuration.
- **Address interleaving mismatch** — if the two DDR controllers have overlapping or non-covering address ranges, some addresses are unmapped. Requests to those addresses produce cryptic "no match for address" errors deep inside the directory controller.
- **Wrong number of LLC slices** — setting `--num-l3caches=2` instead of 16 creates a system where all coherence traffic funnels through two HN-F nodes. The mesh topology is wasted — it becomes a de facto two-node system with 14 idle routers.

## Primary Code Anchors

- `configs/topologies/CustomMesh.py` — the topology that wires CHI nodes into a Garnet mesh.
- `configs/ruby/CHI_config.py` — base classes for all CHI node types (`CHI_RNF`, `CHI_HNF`, `CHI_SNF_MainMem`, etc.) and default NoC parameters.
- `configs/example/noc_config/2x4.py` — the starting template for the 4×4 noc_config file.
- `configs/ruby/CHI.py` — the legacy CHI system builder that instantiates controllers and connects them to the network.
- `tests/gem5/chi_protocol/configs/chi-with-isa.py` — a working CHI system on RISC-V that serves as the primary reference.
- `src/mem/ruby/protocol/chi/CHI.slicc` — the CHI protocol manifest (used as-is, not modified).
- `src/mem/ruby/network/garnet/GarnetNetwork.cc` — the Garnet network (used as-is, not modified).

## Stage 5 — Per-Vnet Dedicated Links in CustomMesh

The baseline system from Stages 1–4 has a bandwidth fidelity gap: every pair of adjacent Garnet routers is connected by a single shared link per direction, and all four CHI virtual networks (REQ, SNP, RSP, DAT) multiplex onto that one link.
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

1. **Without the flag** — rerun the Stage 3 smoke test and confirm identical statistics. Zero behavioral change.
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

1. Written a `noc_config` file that maps 16 CHI nodes onto a 4×4 Garnet mesh with DDR controllers at opposite corners.
2. Assembled a complete system configuration script that wires RISC-V cores, the CHI protocol, a Garnet mesh, and DDR4 memory into a single runnable simulation.
3. Built, booted, and run a multicore test on the 16-core system.
4. Read the resulting statistics — protocol, network, and DRAM — and explained what the numbers mean in terms of the system's topology and architecture.
5. Extended `CustomMesh.py` to support per-vnet dedicated links, closing a bandwidth fidelity gap between gem5's CHI mesh model and real ARM CMN hardware.

The reader finishes the book having both assembled a research-grade tiled multicore simulation and improved the simulator itself — the full arc from consumer to contributor.
