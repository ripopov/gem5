# NoC Under Test — 4×4 CHI Mesh DUT

This document describes the Design Under Test (DUT) exercised by the
`rbook_testbench_gem5` testbench: a **CPU-less, 16-tile CHI mesh** built on
gem5's Ruby coherence framework. It is the reference NoC used to study
Network-on-Chip behavior (latency, bandwidth, congestion, coherence traffic)
in isolation from any RISC-V instruction stream.

The testbench reuses the Chapter-17 CHI/SLICC protocol stack verbatim and is
built with `scons build/RISCV/gem5.opt PROTOCOL=CHI`. All traffic is generated
by lightweight `ChiSeqDriver` engines instead of real cores, which lets a
single deterministic run stress the mesh with precisely controlled access
patterns.

---

## 1. Purpose and Scope

The DUT answers one question: **how does a 4×4 CHI mesh behave under
controlled, repeatable traffic?** It is deliberately *not* a full SoC model.
There are no CPUs, no caches above the leaf, no boot ROM activity, and no DMA
devices. Each tile is a minimal CHI request node attached directly to one mesh
router, so that every observed cycle of latency or byte of bandwidth is
attributable to the network and the coherence protocol — not to a CPU
pipeline.

Two interchangeable Ruby network models sit underneath the same topology:

- **garnet** (default): a detailed flit/router/virtual-channel model. The
  driver auto-sizes the link width so that a full CHI data beat fits in a
  single flit, and enables one physical link per virtual network.
- **simple** (`SimpleNetwork`): an analytical switch/link model with
  per-vnet physical channels.

Running identical scenarios against both models isolates *protocol* effects
from *micro-architectural network* effects.

---

## 2. Top-Level Topology

The mesh is a **4×4 grid of 16 routers**, numbered row-major 0–15. Every
router hosts exactly one **tile** (the traffic-generating request node) and
one **HNF** (Home Node, an LLC/directory slice). Two routers additionally host
a main-memory node, and router 0 hosts the (idle) Misc Node.

```
        col 0      col 1      col 2      col 3
      +--------+ +--------+ +--------+ +--------+
row 0 |   R0   |=|   R1   |=|   R2   |=|   R3   |
      | T0 H0  | | T1 H1  | | T2 H2  | | T3 H3  |
      | MN SNF | |        | |        | |        |
      +--------+ +--------+ +--------+ +--------+
          ||         ||         ||         ||
      +--------+ +--------+ +--------+ +--------+
row 1 |   R4   |=|   R5   |=|   R6   |=|   R7   |
      | T4 H4  | | T5 H5  | | T6 H6  | | T7 H7  |
      +--------+ +--------+ +--------+ +--------+
          ||         ||         ||         ||
      +--------+ +--------+ +--------+ +--------+
row 2 |   R8   |=|   R9   |=|  R10   |=|  R11   |
      | T8 H8  | | T9 H9  | |T10 H10 | |T11 H11 |
      +--------+ +--------+ +--------+ +--------+
          ||         ||         ||         ||
      +--------+ +--------+ +--------+ +--------+
row 3 |  R12   |=|  R13   |=|  R14   |=|  R15   |
      |T12 H12 | |T13 H13 | |T14 H14 | |T15 H15 |
      |        | |        | |        | | SNF    |
      +--------+ +--------+ +--------+ +--------+

  ==  bidirectional inter-router mesh link (horizontal)
  ||  bidirectional inter-router mesh link (vertical)
  Tn  = tile n (ChiSeqDriver + RubySequencer + CHI_TileCacheController)
  Hn  = HNF slice n (LLC + snoop filter / directory)
  MN  = Misc Node (DVM coordinator; idle in this testbench)
  SNF = System Node, main-memory controller (routers 0 and 15)
```

Routing is deterministic **XY (dimension-order)**: a packet travels in the X
dimension first, then Y. Link traversal weights `[1, 1, 2, 2]` (E, W, N, S)
bias the order so that the network is deadlock-free across the four CHI virtual
networks.

### Node placement summary

| Node class       | Count | Routers          | Role                                   |
| ---------------- | ----- | ---------------- | -------------------------------------- |
| Tile (request)   | 16    | 0–15 (one each)  | Traffic generator + leaf cache / DMA   |
| HNF (home)       | 16    | 0–15 (one each)  | LLC slice, snoop filter, directory     |
| SNF (main mem)   | 2     | 0 and 15         | DRAM controllers (interleaved ranges)  |
| MN (misc)        | 1     | 0                | DVM/barrier coordinator (idle in SE)   |

Placement is defined in the CHI NoC config
[`configs/example/noc_config/rbook_4x4.py`](../../../configs/example/noc_config/rbook_4x4.py). The two memory controllers sit at
diagonally opposite corners (0 and 15) so that memory traffic is spread across
the mesh rather than funneled to a single edge.

### How the 4×4 shape is fixed: `parser.set_defaults`

The DUT's overall shape is **not hard-coded** — it is stamped onto gem5's stock
Ruby option parser. The top-level config
[`driver/rbook_testbench_gem5.py`](driver/rbook_testbench_gem5.py) first calls
`Ruby.define_options(parser)` to register all standard Ruby/network flags, then
calls **`parser.set_defaults(...)`** to override their defaults with the values
this testbench requires. This is the single most important configuration
statement in the driver: it is what turns a generic CHI/Ruby invocation into
*this specific* 4×4 mesh DUT without the user having to pass a long flag list.

The defaults it pins are:

| Default set via `set_defaults` | Value | Effect on the DUT |
| ------------------------------ | ----- | ----------------- |
| `num_cpus` | 16 | 16 tiles (one per router) |
| `num_l3caches` | 16 | 16 HNF/LLC slices (one per router) |
| `num_dirs` | 2 | 2 main-memory SNF nodes |
| `topology` | `CustomMesh` | mesh builder that honors `rbook_4x4.py` placement |
| `network` | `garnet` | default network model |
| `chi_config` | `rbook_4x4.py` | the 4×4 node→router placement file |
| `mem_size` | `512MiB` | physical memory range |
| `simple_physical_channels` | `True` | one channel per vnet (SimpleNetwork) |
| `per_vnet_links` | `True` | one mesh link per vnet (garnet) |
| `vcs_per_vnet` | `8` | virtual channels per vnet (garnet) |

Because these are *defaults* (not forced assignments), any one of them can still
be overridden on the command line — e.g. `--network=simple` swaps the network
model while leaving the rest of the 4×4 shape intact.

---

## 3. Anatomy of a Tile

Unlike a stock CHI system, a tile here has **no L1, no CPU, and no side
router**. The request node connects *directly* to its mesh router through a
single external link. This keeps the request injection path as short and as
observable as possible.

```
   +-------------------------------------------------+
   |                     Tile n                      |
   |                                                 |
   |   ChiSeqDriver  (ClockedObject, fiber engine)   |
   |        |                                         |
   |        | RequestPort  "port"                     |
   |        v                                         |
   |   RubySequencer  (_cpu_ports[n])                 |
   |        |                                         |
   |        v                                         |
   |   CHI_TileCacheController                        |
   |     (Base_CHI_Cache_Controller, is_HN=False)    |
   |     - rnf_l2 mode: 256KiB 8-way coherent L2      |
   |     - rni  mode: cache-less DMA shim             |
   |        |                                         |
   |        |  reqOut/rspOut/snpIn/datIn/datOut ...   |
   |        v                                         |
   |   ====  single ExtLink to mesh Router n  ====    |
   +-------------------------------------------------+
```

### Request-generation path

1. **ChiSeqDriver** is a `ClockedObject` that runs a *fiber-backed* C++ traffic
   sequence (see Section 6). It exposes one `RequestPort` named `port`.
2. The driver's port binds to **`system.ruby._cpu_ports[n].in_ports`**, i.e.
   the tile's **RubySequencer**, which translates memory packets into CHI
   transactions.
3. The sequencer feeds the **CHI_TileCacheController**, a single
   `Base_CHI_Cache_Controller` parameterized by the `--rn-mode` flag.
4. The controller drives the four CHI message channels onto the mesh through
   one external link to its router.

### Tile modes (`--rn-mode`)

| Mode            | Cache                          | Coherence behavior                                                   | CHI transactions issued                |
| --------------- | ------------------------------ | -------------------------------------------------------------------- | -------------------------------------- |
| `rnf_l2` (def.) | 256 KiB, 8-way, 6cy data/2cy tag | Full participant: allocates, evicts, answers snoops, self-downgrades | ReadShared, ReadUnique, CleanUnique, snoop responses |
| `rni`           | 128 B dummy, no allocation     | None: nothing cached, no evictions, no snoop participation           | ReadOnce, WriteNoSnpFull               |

`rnf_l2` models a coherent leaf cache (the interesting case for coherence
traffic and snoop fan-out). `rni` models a DMA-like, cache-less initiator that
exercises the network with non-coherent point-to-point traffic. Both modes are
defined in [`driver/cfg_rn.py`](driver/cfg_rn.py) (class `CHI_TileCacheController`).

A subtle implementation detail: the tile node class (`CHI_Tile`, also in
`cfg_rn.py`) inherits from the stock `CHI_RNI_DMA` node type. This is *purely*
for topology classification — it makes `CustomMesh` treat the tile as a
direct-attach node and **skip the extra "side router"** that a normal RNF would
receive. The result is the clean one-link-to-router wiring shown above.

---

## 4. The Home Node (HNF) and Memory

Each router also hosts one **HNF** slice. Collectively the 16 HNFs form a
distributed, address-interleaved last-level cache plus the coherence point
(snoop filter / directory) for every cache line. A request from any tile is
routed to the HNF that *homes* the target line; that HNF resolves coherence
(issuing snoops to sharers as needed) and, on a miss, forwards to main memory.

### Address interleaving

Lines are striped across HNFs by **cache-line granularity** using address bits
just above the block offset:

```
  63 ........ 10 | 9   8   7   6 | 5 ........ 0
  +-------------+---------------+-------------+
  |  line index  |  HNF select   | byte offset |
  |  within HNF  |  (4 bits)     |  (64 B = 6)  |
  +-------------+---------------+-------------+
```

- Bits **[5:0]** select the byte within a 64-byte line.
- Bits **[9:6]** select one of the 16 HNFs (4 bits).
- Bits **[63:10]** index lines within a given HNF's slice; the stride between
  consecutive lines homed at the *same* HNF is `64 B × 16 = 1024 B`.

This is exactly the scheme reproduced by [`driver/address_planner.py`](driver/address_planner.py)
(`MeshLayout` / `AddressPlanner`), which gives scenarios precise control over
*which* HNF a given address maps to — essential for constructing controlled
contention (e.g. forcing two tiles to share a line homed at a chosen router) or
spreading traffic evenly (one line per HNF).

Main memory behind the HNFs is served by **2 SNF (System Node) controllers** at
routers 0 and 15, each owning an interleaved half of the physical address
range.

---

## 5. Network Configuration

The topology, latencies, and link layout are defined in
[`configs/example/noc_config/rbook_4x4.py`](../../../configs/example/noc_config/rbook_4x4.py). The same file is consumed by both
network models; only the relevant subset of parameters applies to each.

### Virtual networks

CHI uses **4 virtual networks (vnets)** to keep message classes independent and
the protocol deadlock-free:

| vnet | Class    | Example messages                                  |
| ---- | -------- | ------------------------------------------------- |
| 0    | Request  | ReadShared, ReadUnique, ReadOnce, WriteNoSnpFull  |
| 1    | Snoop    | SnpShared, SnpUnique, SnpOnce                      |
| 2    | Response | Comp, CompDBIDResp, CompAck, RetryAck, PCrdGrant   |
| 3    | Data     | DataResp, SnpRespData, write data                  |

With `per_vnet_links = True`, garnet instantiates one physical mesh link per
vnet, so the four classes never contend for the same wire. `vcs_per_vnet = 8`
gives each vnet eight virtual channels for head-of-line-blocking avoidance.

### Latencies and bandwidth

| Parameter (in `rbook_4x4.py`) | Value | Meaning                                                   |
| ----------------------------- | ----- | --------------------------------------------------------- |
| `num_rows` × `num_cols`       | 4 × 4 | 16 routers                                                |
| `router_latency`             | 4 cy  | Router pipeline (1 in + 2 route + 1 out)                  |
| `router_link_latency`        | 2 cy  | Inter-router (hop-to-hop) link delay                      |
| `node_router_latency`        | 2 cy  | Node-to-router external link delay                        |
| `router_buffer_size`         | 8     | `SimpleNetwork` per-port buffer depth                     |
| `link_bandwidth_factor`      | 40    | `SimpleNetwork` bytes/cycle per link                      |

The `router_buffer_size` default is deliberately raised from gem5's stock value
of 4 to **8**: the column-0 incast pattern (traffic converging on the memory
controller at router 0) starves with shallow buffers, and 8 keeps the funnel
fed up to the structural bandwidth ceiling.

### Garnet single-flit sizing

CHI data packets carry a 32-byte data payload plus an 8-byte control header
(40 bytes total). For garnet to deliver a data beat as a **single flit**, the
link must be at least `40 × 8 = 320 bits` wide. The driver
([`driver/rbook_testbench_gem5.py`](driver/rbook_testbench_gem5.py)) auto-bumps `--link-width-bits` to this
minimum whenever the user has not explicitly overridden it, so that flit-level
results reflect one-flit data transfers rather than artificial fragmentation.

### Link inventory (per vnet)

For a 4×4 mesh:

- Horizontal inter-router links: `4 rows × 3 = 12` (bidirectional).
- Vertical inter-router links: `3 × 4 cols = 12` (bidirectional).
- External node-to-router links: one per attached controller (16 tiles +
  16 HNFs + 2 SNFs + 1 MN).

With `per_vnet_links`, the 24 inter-router bidirectional links are replicated
across the 4 vnets.

---

## 6. Traffic Generation Engine

The C++ side lives under [`src/chi_testbench_gem5/`](../../../src/chi_testbench_gem5/). Its job is to inject
precisely controlled, reproducible CHI traffic and to coordinate tiles with
each other.

### ChiSeqDriver and fibers

`ChiSeqDriver` (a `ClockedObject`) owns a **`SeqThread`**, a gem5 `Fiber`. When
the simulation starts, the driver schedules a kick event that runs the fiber;
the fiber executes a `ChiSequence::run()` body. Whenever the sequence issues a
blocking memory operation or waits on time, the fiber **yields back to the gem5
event loop**; when the response (or timed wake) arrives, the driver resumes the
fiber exactly where it left off. This gives sequence authors a natural,
straight-line imperative programming model on top of gem5's discrete-event
engine, without callbacks.

The driver offers both **blocking** operations (`read`, `write`,
`read_exclusive`) and a **non-blocking, ordered** API
(`async_*_req` / `try_read_resp`) used to keep multiple requests in flight. The
`--num-outstanding-reqs` flag (default **4**) bounds the in-flight depth and
maps directly to the controller's TBE (transaction buffer entry) count, so it
controls both injection pressure and how much MSHR-style parallelism the tile
exposes.

### Sequences (the workloads)

`ChiSequence` is an abstract `SimObject`; each concrete sequence is its own
strongly-typed SimObject with its own parameters. Two are provided:

- **MemsetSequence** — streams writes (or reads) across a contiguous,
  per-tile, *non-overlapping* address range. Supports an L3 warm-up phase, a
  measured **ROI** (middle 80% of the range, bracketed by ramp-up/ramp-down),
  and per-core bandwidth reporting. Partial writes (e.g. 63 of 64 bytes) force
  `ReadUnique`; full-line stores warm the LLC.

- **PingPongSequence** — two tiles repeatedly hand off a single shared cache
  line: one writes, the other spins reading until it observes the new value,
  then writes back. This maximizes coherence round-trips and measures
  inter-tile latency under contention.

### Cross-tile synchronization

Two `SimObject` primitives in `src/chi_testbench_gem5/sync/` coordinate the
fibers across tiles:

- **ChiGem5Barrier** — a counter that triggers `exitSimLoop` (or a phase
  transition) once `expected` participants have signaled. Used to align ROI
  start/stop across all active tiles and to detect overall completion.
- **Latch / ChiEventBus** — a 1-to-1 notify/wait primitive (with a pending
  flag for order-independent rendezvous) and a named 1-to-N registry. Used by
  ping-pong-style hand-offs.

All wake-ups across fibers go through a cross-fiber scheduling helper so that a
notifying fiber never resumes another fiber directly — it schedules a wake event
on the target driver instead, keeping the fiber stack discipline intact.

---

## 7. End-to-End Transaction Flow

A representative coherent read from tile *s* to a line homed at HNF *h*
(`rnf_l2` mode, on a miss requiring a snoop):

```
  Tile s                 Router s ... Router h          HNF h            Sharer tile k        SNF (mem)
  ChiSeqDriver
    | read(addr)
    v
  RubySequencer
    | (CHI ReadShared on vnet 0)
    v
  CHI_TileCacheController --ExtLink--> [ XY route across mesh, vnet 0 ] --> HNF h
                                                                            | lookup / snoop filter
                                                                            | (SnpShared on vnet 1)
                                                                            +-----------------------> Sharer tile k
                                                                            |                          | snoop resp
                                                                            | <----- vnet 2/3 ---------+ (+ data)
                                                                            | (miss -> fetch)
                                                                            +------ vnet 0 -----------------------> SNF
                                                                            | <----- vnet 3 (data) ----------------+
    | <-------------------- DataResp on vnet 3 ----------------------------- |
    v
  fiber resumes, read() returns
```

The key observation: **every arrow is a real mesh traversal** with router and
link latency, and each message class rides its own vnet. Because there are no
CPUs in the path, the measured latency is precisely the sum of injection,
routing, coherence resolution, and (on a miss) memory access.

---

## 8. Flow Control and Backpressure

The HNF's request-input handling is selectable via `--allow-retryack`
(default **1**):

- **`1` (RetryAck):** the HNF uses an effectively unbounded request buffer and
  the standard CHI credit protocol (`RetryAck` / `PCrdGrant`). When the HNF is
  full, it bounces requests and re-admits them via credits. This models a
  well-provisioned home node.
- **`0` (backpressure):** the HNF disables retry and uses a shallow (2-entry)
  request buffer, so a full TBE table **stalls the input channel** and pushes
  backpressure into the network. This models an RTL-style LLC without a retry
  pool and is the mode of interest for studying congestion propagation.

A raised `--deadlock-threshold` (default **5,000,000** cycles) keeps the
sequencer's deadlock detector quiet despite the deliberately sparse, bursty
traffic.

---

## 9. Configuration File Map

| File | Role |
| ---- | ---- |
| [`driver/rbook_testbench_gem5.py`](driver/rbook_testbench_gem5.py) | Top-level config: pins the 4×4 shape via `parser.set_defaults` over the stock Ruby options, builds the `System`, sizes garnet links, loads a scenario, swaps in tile/MN factories via `system._rnf_gen` / `system._mn_gen`, applies flow-control and deadlock overrides. |
| [`driver/cfg_rn.py`](driver/cfg_rn.py) | Per-tile request-node wiring: `CHI_TileCacheController` (rni / rnf_l2 modes) and the `CHI_Tile` node that attaches directly to a mesh router. |
| [`driver/address_planner.py`](driver/address_planner.py) | Cache-line address math: HNF interleaving, per-HNF strides, helpers for one-line-per-HNF and diagonal (max-hop) address pairs. |
| [`configs/example/noc_config/rbook_4x4.py`](../../../configs/example/noc_config/rbook_4x4.py) | CHI NoC parameters: 4×4 geometry, node→router placement, router/link latencies, buffer sizes, bandwidth. |
| [`scenarios/memset.py`](scenarios/memset.py) | Builds one `ChiSeqDriver` per tile running `MemsetSequence`; active tiles write non-overlapping ranges. |
| [`scenarios/ping_pong.py`](scenarios/ping_pong.py) | Builds two active `ChiSeqDriver`s exchanging one shared line (default homed at HNF 7); other tiles idle. |
| [`src/chi_testbench_gem5/`](../../../src/chi_testbench_gem5/) | C++ engine: `ChiSeqDriver`, fiber `SeqThread`, sequences (`sequences/`), and sync primitives (`sync/`). |
| [`Makefile`](Makefile) | `run-memset` / `run-ping_pong` / `run-all` targets; honors `RN_MODE`, `ACTIVE_CORES`, `NUM_OUTSTANDING_REQS`. |

---

## 10. Key Command-Line Knobs

| Flag | Default | Effect on the DUT |
| ---- | ------- | ----------------- |
| `--scenario` | `memset` | Selects the workload builder under `scenarios/`. |
| `--active-cores` | `all` | Which tiles inject traffic (subset or all 16). |
| `--operation` | `store` | memset ROI access type: `store` → ReadUnique; `load` → ReadShared. |
| `--num-outstanding-reqs` | `4` | In-flight depth per tile; sets controller TBE count. |
| `--rn-mode` | `rnf_l2` | Coherent L2 leaf cache vs. cache-less DMA tile. |
| `--allow-retryack` | `1` | HNF retry-credit flow control vs. shallow-buffer backpressure. |
| `--deadlock-threshold` | `5,000,000` | Sequencer deadlock detection window (cycles). |
| `--network` | `garnet` | Detailed flit model vs. analytical `simple` network. |

---

## 11. Running the DUT

From `ruby-book/final/chi_testbench_gem5/`:

- `make run-memset` — streaming write/read bandwidth test across all tiles.
- `make run-ping_pong` — two-tile shared-line coherence latency test.
- `make run-all` — both scenarios in sequence.

Override defaults with `make RN_MODE=rni run-memset`,
`make ACTIVE_CORES=0,15 run-ping_pong`, or
`make NUM_OUTSTANDING_REQS=8 run-all`. Each invocation writes a timestamped
`m5out/` directory (per the repository's timeout-wrapper convention) and leaves
a `last-<scenario>-<mode>` symlink to the most recent run.

---

## 12. Why This DUT Is a Good NoC Probe

- **No CPU noise.** Latency and bandwidth numbers are attributable to the
  network and coherence protocol alone.
- **Deterministic, addressable traffic.** The address planner lets a scenario
  target a specific HNF or build a specific hop distance, so contention and
  routing effects are constructed on purpose, not by chance.
- **Two network models, one topology.** garnet vs. simple comparisons separate
  flit-level micro-architecture from analytical link behavior.
- **Tunable pressure.** Outstanding-request depth, active-tile count, retry vs.
  backpressure, and rni vs. rnf_l2 modes span the space from light point-to-
  point traffic to heavy incast with congestion propagation.

Together these make the 4×4 CHI mesh a clean, reproducible bench for studying
on-chip interconnect behavior.
