# Memory Architecture and NoC Modeling in gem5
## From Requests to Routers to DRAM

### Goal

This book teaches the reader to read, modify, validate, and extend gem5's memory-system and NoC models.
It is not a survey of architecture ideas detached from implementation.
It is a guided walk through the actual gem5 codebase.

### Reader Profile

- Comfortable reading C++ and Python.
- Has completed an introductory computer architecture course.
- Wants to become genuinely productive in gem5 memory, coherence, NoC, and DRAM modeling.

### Book-Level Commitments

- Every chapter opens with a concrete problem, failure, or measurement question.
- Every chapter has three explicit layers: `Intuition`, `Working Model`, and `Formal and Code`.
- Every chapter names primary code anchors in the tree and at least one runnable artifact.
- Every mechanism is paired with a failure mode and a debugging workflow.
- Every core chapter includes a short `How We Know This` section tying claims to source code, statistics methodology, or primary papers.
- Every chapter ends with `Key Ideas`, `1-Page Mental Model`, `Common Misconceptions`, `If You Remember One Thing`, and exercises.
- Optional deep dives are labeled explicitly instead of being mixed into the core path.
- Terminology is consistent.
- In prose, we say `cache line`, and we map that term to code names such as `CacheBlk`, `MessageBuffer`, `RubySequencer`, `packet`, `flit`, and `tick` when needed.
- Quantitative claims and design tradeoffs are tied either to source code, reproducible experiments, or primary papers.

### Deliberate Scope Decisions

- The core path uses only examples that already exist in the repository.
- The book does not depend on external benchmark suites for its main labs.
- Traffic generators, `RubyTester`, and `GarnetSyntheticTraffic` are the default laboratory tools because they are reproducible and already exercised in-tree.
- External-memory integrations such as DRAMSys, DRAMSim, and HMC are treated as advanced optional material, not as prerequisites for mastering gem5's core memory path.

### Primary Running System

The main spine is one evolving timing-mode RISC-V multicore system.
It starts as a generator-to-memory path, grows into a Classic hierarchy, then a Ruby hierarchy, then a routed on-chip network, then a CHI-based fabric, and finally a detailed memory-controller study.
We keep asking the same questions as the system evolves: where did the request wait, who owned the line, which queues filled, which links saturated, and which timing model dominated the result.

```text
Chapter 1: Generator or CPU -> memory
Chapter 3: Core -> private L1 -> shared L2 -> memory
Chapter 6: Core -> RubySequencer -> controller -> directory -> memory
Chapter 10: Core -> Ruby -> Garnet routers and links -> directory -> memory
Chapter 13: Core -> CHI RN-F or L1 -> HN-F -> SN-F -> DRAM
Chapter 14: Core -> cache hierarchy -> address mapping -> MemCtrl and DRAMInterface
```

### Reusable Experiment Harnesses

- `tests/gem5/traffic_gen/configs/simple_traffic_run.py` and `configs/example/gem5_library/memory_traffic.py` for controlled latency and bandwidth sweeps across cache hierarchies and memories.
- `configs/example/ruby_random_test.py`, `configs/example/ruby_mem_test.py`, and `src/cpu/testers/rubytest/*` for protocol validation and race hunting.
- `configs/example/garnet_synth_traffic.py` and `src/cpu/testers/garnet_synthetic_traffic/*` for network saturation, routing, and deadlock studies.
- `tests/gem5/chi_protocol/configs/chi-with-isa.py` for a modern stdlib CHI system that already runs on RISC-V, Arm, and x86 in-tree.

### Legacy and Modern Configuration Paths

gem5 currently exposes two configuration stories that both matter for this book.
The legacy path is centered on `configs/ruby/*.py`, `configs/network/Network.py`, and `configs/topologies/*.py`.
The modern path is centered on `src/python/gem5/components/cachehierarchies/*`, `src/python/gem5/components/memory/*`, and the stdlib-style test and example configurations in `tests/gem5/*` and `configs/example/gem5_library/*`.
The book will teach both, and it will make the relationship between them explicit instead of pretending one superseded the other completely.

### Chapter Template

Every chapter in the final book should follow the same structure.
That consistency is part of the learning strategy, not just presentation polish.

- Opening failure or measurement question.
- Minimal working version first, then added realism, then optimizations, then edge cases.
- `Intuition` section with a diagram before formalism.
- `Working Model` section with step-by-step execution semantics.
- `Formal and Code` section tied to exact files in the tree.
- `How We Know This` section with methodology, statistics, and primary references.
- `Failure Mode` section that shows where naive reasoning breaks.
- `Experiment` section using one of the reusable harnesses above.
- Chapter ending compression page with key ideas, misconceptions, and exercises.

---

## Part I — The Memory Path Before Ruby

### Chapter 1: Why Memory Dominates the Story

> *The core is fast until the first long-latency miss turns the machine into a queueing problem.*

- Core question: Why does memory behavior dominate simulated performance long before the core model looks complicated?
- Primary code anchors: `tests/gem5/traffic_gen/configs/simple_traffic_run.py`, `configs/example/gem5_library/memory_traffic.py`, `src/python/gem5/components/cachehierarchies/classic/no_cache.py`.
- Runnable anchors: `LinearGenerator`, `RandomGenerator`, and `GUPSGenerator*` in `tests/gem5/traffic_gen/configs/simple_traffic_run.py`.
- Failure focus: Average latency alone hides saturation, burstiness, and row-buffer effects.
- Outcome: The reader can reproduce a memory-only baseline and explain why queueing and service time matter before coherence even enters the picture.

### Chapter 2: How gem5 Moves a Memory Request

> *Before studying caches or protocols, we need to know what a request is, who owns time, and which object wakes up next.*

- Core question: How does a memory request move through gem5's event-driven substrate?
- Primary code anchors: `src/sim/main.cc`, `src/sim/eventq.hh`, `src/sim/eventq.cc`, `src/sim/sim_object.hh`, `src/sim/sim_object.cc`, `src/sim/init.hh`, `src/sim/init.cc`, `src/mem/request.hh`, `src/mem/packet.hh`, `src/mem/packet.cc`, `src/mem/port.hh`, `src/mem/port.cc`, `src/mem/protocol/atomic.hh`, `src/mem/protocol/timing.hh`, `src/mem/protocol/functional.hh`, `src/python/pybind11/port.cc`.
- Runnable anchors: Minimal no-cache runs from Part I and short instrumented packet traces.
- Failure focus: Mixing up atomic, timing, and functional semantics leads to experiments that look valid but answer the wrong question.
- Outcome: The reader can trace one request from Python configuration through SimObjects, ports, packets, and the event queue.

### Chapter 3: Classic Caches in C++

> *The simplest interesting memory system in gem5 is still a real machine full of tags, queues, and backpressure.*

- Core question: How does the Classic cache path implement hits, misses, fills, evictions, and snoops?
- Primary code anchors: `src/mem/cache/base.hh`, `src/mem/cache/base.cc`, `src/mem/cache/cache.hh`, `src/mem/cache/cache.cc`, `src/mem/cache/cache_blk.hh`, `src/mem/cache/cache_blk.cc`, `src/mem/cache/mshr.hh`, `src/mem/cache/mshr.cc`, `src/mem/cache/mshr_queue.hh`, `src/mem/cache/write_queue.hh`, `src/mem/coherent_xbar.hh`, `src/mem/coherent_xbar.cc`, `src/python/gem5/components/cachehierarchies/classic/private_l1_private_l2_cache_hierarchy.py`.
- Runnable anchors: `PrivateL1` and `PrivateL1PrivateL2` in `tests/gem5/traffic_gen/configs/simple_traffic_run.py`.
- Failure focus: MSHR exhaustion, write-buffer pressure, and snoop traffic can dominate behavior even when hit rate looks healthy.
- Outcome: The reader can explain the Classic miss path at the level of concrete queues and methods, not just cache diagrams.

### Chapter 4: Replacement, Prefetching, and Measurement Discipline in the Classic Path

> *A cache hierarchy can be “better” on paper and worse in the simulator because the wrong secondary effect wins.*

- Core question: How do replacement and prefetch policies change behavior, and how should we measure them without fooling ourselves?
- Primary code anchors: `src/mem/cache/replacement_policies/*`, `src/mem/cache/prefetch/*`, `src/mem/cache/tags/base_set_assoc.hh`, `src/mem/cache/tags/base_set_assoc.cc`, `src/mem/cache/tags/indexing_policies/*`, `tests/gem5/replacement_policies/configs/cache_hierarchies.py`.
- Runnable anchors: Classic cache sweeps with `tests/gem5/traffic_gen/configs/simple_traffic_run.py`.
- Failure focus: Hit rate without traffic shape, queue occupancy, or bandwidth accounting is often misleading.
- Outcome: The reader can design a clean micro-study of a Classic hierarchy and justify the chosen metrics.

---

## Part II — Ruby and Coherence

### Chapter 5: Why Ruby Exists — From Classic Limitations to Protocol State Machines

> *The Classic cache hierarchy got you surprisingly far — until you needed to change the coherence protocol. Ruby exists because protocols are too important to hard-code.*

- Core question: Why does gem5 have two memory systems, what does Ruby buy you over Classic caches, and what do SWMR and the data-value invariant look like in real protocol code?

#### Section 1 — The Bridge from Classic to Ruby

- **Motivation through limitation**: Start from the system the reader already knows (Classic caches from Chapters 3–4). Show what happens when you try to answer questions Classic can't: "What if I want three-level inclusive coherence?", "What if I need an Owned state?", "What if I want to model a directory protocol instead of snooping?" The Classic path hard-codes its protocol in C++ (`Cache::handleSnoop`, `CoherentXBar` broadcast); changing the protocol means rewriting the simulator, not just the configuration.
- **Two memory systems, one simulator**: Explain the architectural decision — Classic optimizes for speed and simplicity with a fixed snooping protocol; Ruby optimizes for protocol flexibility via SLICC-generated state machines, at the cost of more setup and slower simulation. They share the same Port interface but are otherwise independent stacks. A simulation uses one or the other, never both.
- **Ruby at 10,000 feet**: Before any code, give the reader a mental map of Ruby's moving parts — controllers (cache, directory, DMA), a network that carries messages between them, a sequencer that bridges CPU ports to Ruby's protocol world, and SLICC as the language that defines what each controller does. Diagram: CPU → Sequencer → L1 Controller ↔ Network ↔ Directory Controller → Memory. Contrast with Classic's CPU → Cache → XBar → Cache → XBar → Memory.
- **What SLICC is and isn't**: SLICC is a domain-specific language for specifying coherence protocol state machines — it is *not* a general-purpose language. It compiles `.sm` files into C++ controller classes. The reader writes states, events, transitions, and actions; SLICC generates the `doTransition()` dispatch, the `wakeup()` loop, statistics, and HTML documentation. Establish this framing so the reader knows what to expect in the rest of the chapter.

#### Section 2 — Coherence Invariants and Reading MI_example

- **Coherence from first principles**: SWMR (single-writer / multiple-reader) and the data-value invariant. Present as contracts that every protocol must enforce, not as definitions to memorize. "If these break, what goes wrong?" — concrete stale-data and lost-update scenarios on a two-core trace.
- **Reading MI_example as a first protocol**: Walk through `MI_example-cache.sm` and `MI_example-dir.sm` state by state. MI has only two stable states (Modified, Invalid) — small enough to hold in your head entirely. Trace a load miss, a store hit, and an eviction through the state machine. Show the `.slicc` manifest file that ties the protocol together.
- Primary code anchors: `src/mem/ruby/protocol/MI_example.slicc`, `src/mem/ruby/protocol/MI_example-msg.sm`, `src/mem/ruby/protocol/MI_example-cache.sm`, `src/mem/ruby/protocol/MI_example-dir.sm`.

#### Section 3 — Building MSI from Scratch

- **Construction walkthrough**: Build a working MSI protocol file by file: `MSI-msg.sm` (message types) → `MSI-cache.sm` (states, events, actions, transitions — adding the Shared state to MI) → `MSI-dir.sm` (directory with `DirectoryMemory`, `NetDest` sharer bitvector, lazy entry allocation, transient states for memory latency) → `.slicc` manifest → Kconfig registration → build → run `simple_ruby.py` → interpret output.
- **SLICC gotchas surfaced in context**: Each gotcha is introduced at the point the reader hits it: hard-coded `mandatoryQueue` name, SLICC name-mangling (`TBE` → `L1Cache_TBE`), mandatory `setMRU()` calls, `dequeue()` delayed one cycle, `MessageSize` required in every message type.
- Primary code anchors: `src/learning_gem5/part3/MSI.slicc`, `src/learning_gem5/part3/MSI-cache.sm`, `src/learning_gem5/part3/MSI-dir.sm`, `src/learning_gem5/part3/MSI-msg.sm`, `configs/learning_gem5/part3/simple_ruby.py`, `configs/learning_gem5/part3/msi_caches.py`, `configs/learning_gem5/part3/ruby_caches_MI_example.py`.

#### Section 4 — Build, Run, Verify

- **Explicit code-build-run-debug cycle**: The chapter closes with a full loop — build the MSI protocol, run it with `simple_ruby.py`, read the protocol trace output, verify correctness on a known sharing pattern. References Appendix A for build details.
- Runnable anchors: `configs/learning_gem5/part3/simple_ruby.py` and MI-based experiments through the traffic-generator harness.

- Failure focus: Missing transitions and stale-data scenarios are easier to understand in minimal protocols than in production ones. Also: the failure mode of *choosing* Classic when you needed Ruby (or vice versa) — wasted effort from picking the wrong memory system for your research question.
- Outcome: The reader understands why Ruby exists alongside Classic, can navigate Ruby's high-level architecture, can read a small protocol state machine, predict its behavior on a short sharing trace, and has built a working three-file MSI protocol from scratch.

### Chapter 6: Ruby Architecture and Message Flow

> *Ruby is not “just coherence”; it is a specific controller-network-message architecture with concrete storage and queueing objects.*

- Core question: How do Ruby controllers, sequencers, storage structures, and message buffers fit together?
- Primary code anchors: `configs/ruby/Ruby.py`, `src/mem/ruby/system/RubySystem.hh`, `src/mem/ruby/system/RubySystem.cc`, `src/mem/ruby/system/RubyPort.hh`, `src/mem/ruby/system/RubyPort.cc`, `src/mem/ruby/system/Sequencer.hh`, `src/mem/ruby/system/Sequencer.cc`, `src/mem/ruby/system/DMASequencer.hh`, `src/mem/ruby/system/DMASequencer.cc`, `src/mem/ruby/network/MessageBuffer.hh`, `src/mem/ruby/network/MessageBuffer.cc`, `src/mem/ruby/slicc_interface/AbstractController.hh`, `src/mem/ruby/slicc_interface/AbstractController.cc`, `src/mem/ruby/structures/CacheMemory.hh`, `src/mem/ruby/structures/CacheMemory.cc`, `src/mem/ruby/structures/DirectoryMemory.hh`, `src/mem/ruby/structures/DirectoryMemory.cc`, `src/mem/ruby/structures/TBETable.hh`, `src/mem/ruby/structures/NetDest.hh`, `src/mem/ruby/structures/NetDest.cc`, `src/mem/ruby/profiler/Profiler.cc`, `configs/learning_gem5/part3/msi_caches.py`, `configs/learning_gem5/part3/ruby_caches_MI_example.py`.
- Runnable anchors: `src/python/gem5/components/cachehierarchies/ruby/mi_example_cache_hierarchy.py` and the legacy `configs/ruby/MI_example.py` path.
- Failure focus: Readers often confuse controller-local buffers, network transport, and backing memory service as one undifferentiated “Ruby latency.”
- Additional sections this chapter must cover:
  - **Message buffer mechanics**: virtual network assignment as a deadlock avoidance mechanism (requests on vnet 0, forwards on vnet 1, responses on vnet 2 with highest priority — responses stuck behind requests cause deadlock). `in_port` declaration order determines processing priority. `isReady(clockEdge())` / `peek` / `enqueue` / `dequeue` patterns and their timing semantics (dequeue takes effect next cycle).
  - **Buffer management strategies**: compare `stall()` (simple but head-of-line blocking), `recycle()` (avoids HOL but wastes bandwidth re-checking), and `stall_and_wait(address)` (best selectivity but most complex). MSI uses stall; production protocols upgrade to stall_and_wait.
  - **Functional access contract**: every controller must implement `getAccessPermission()`, `setAccessPermission()`, `functionalRead()`, `functionalWrite()`. Needed for GDB reads and binary loading via `RubyPortProxy`. `AccessPermission` enums (Invalid, NotPresent, Busy, Read_Only, Read_Write) map to states via auto-generated `*_State_to_permission()`. TBE must also be consulted for transient states.
  - **Ruby structures**: `CacheMemory`, `TBETable`, `DirectoryMemory` (lazy allocation), and `NetDest` (bitvector for sharers/owner with `add`, `remove`, `clear`, `count`, `isElement` API).
  - **Python configuration patterns**: controller version numbering (each type needs unique monotonic `version`), Sequencer creation and linkage, address range assignment for directories, `MessageBuffer` creation with `ordered` parameter and `in_port`/`out_port` network connections, `mandatoryQueue` as a special unconnected buffer, `RubyPortProxy` for functional access, setup order (topology → controllers → sequencers → network → CPU attachment).
- Outcome: The reader can walk one Ruby request from `RubySequencer` to controller wakeup, to message buffer, to directory, and back.

### Chapter 7: SLICC as a Compiler, Not Just a Syntax

> *SLICC is powerful precisely because it generates a controller framework from a disciplined state-machine description.*

- Core question: How does SLICC parse, analyze, and generate the controller code that Ruby executes?
- Primary code anchors: `src/mem/slicc/main.py`, `src/mem/slicc/parser.py`, `src/mem/slicc/symbols/StateMachine.py`, `src/mem/slicc/symbols/Transition.py`, `src/mem/slicc/symbols/Action.py`, `src/mem/slicc/generate/dot.py`, `src/mem/slicc/generate/html.py`, plus the `MI_example` protocol files from Chapter 5.
- Runnable anchors: Small protocol edits in `MI_example` and build regeneration through the normal gem5 build.
- Failure focus: Treating SLICC as magical code generation hides the real constraints on actions, transitions, and controller storage.
- SLICC HTML generation: cover the HTML output (`--html-path` / SLICC_HTML build option) as a first-class compiler artifact alongside C++ controllers and dot graphs. This produces navigable protocol documentation directly from `.sm` files.
- Gotcha explanations: explain *why* each SLICC gotcha from Chapter 5 exists (why the Sequencer hard-codes `mandatoryQueue`, why SLICC name-mangles types, why `dequeue` is delayed one cycle, why error line numbers point after the actual error).
- Outcome: The reader can connect a `.sm` transition to the generated controller behavior it produces.

### Chapter 8: Production Coherence Protocols in gem5

> *Minimal protocols teach the grammar of coherence; production protocols teach the engineering.*

- Core question: How do gem5's three production Ruby protocols differ in architecture, states, and tradeoffs?
- Primary code anchors: `src/mem/ruby/protocol/MESI_Two_Level.slicc`, `src/mem/ruby/protocol/MESI_Two_Level-*.sm`, `src/mem/ruby/protocol/MOESI_CMP_directory.slicc`, `src/mem/ruby/protocol/MOESI_CMP_directory-*.sm`, `src/mem/ruby/protocol/MOESI_CMP_token.slicc`, `src/mem/ruby/protocol/MOESI_CMP_token-*.sm`, `configs/ruby/MESI_Two_Level.py`, `configs/ruby/MOESI_CMP_directory.py`, `configs/ruby/MOESI_CMP_token.py`.
- Runnable anchors: `MESITwoLevel` in `tests/gem5/traffic_gen/configs/simple_traffic_run.py`, `RubyTester` with `configs/example/ruby_random_test.py`.
- Failure focus: Invalidation storms, inclusion pressure, the MT-state forwarding trap, owner bottlenecks in MOESI, token conservation violations, and persistent request storms.
- Covers MESI_Two_Level (inclusive L2 as cache+directory, silent E→M, stall_and_wait), MOESI_CMP_directory (dirty sharing via O state, complex L2 composite states), and MOESI_CMP_token (distributed token counting, persistent requests for starvation avoidance).
- Outcome: The reader understands the architecture, states, message flows, failure modes, and tradeoffs of all three production protocols, and knows when to choose each one.

---

## Part III — NoC Modeling

### Chapter 9: Ruby Networks Before Garnet

> *Not every Ruby network model is cycle-accurate, and that distinction matters.*

- Core question: What is the difference between Ruby message transport in general and cycle-accurate flit transport specifically?
- Primary code anchors: `src/mem/ruby/network/Network.hh`, `src/mem/ruby/network/Network.cc`, `src/mem/ruby/network/simple/SimpleNetwork.hh`, `src/mem/ruby/network/simple/SimpleNetwork.cc`, `configs/network/Network.py`, `configs/topologies/BaseTopology.py`, `src/python/gem5/components/cachehierarchies/ruby/topologies/simple_pt2pt.py`.
- Runnable anchors: Simple-network Ruby experiments and point-to-point stdlib Ruby hierarchies.
- Failure focus: Assigning protocol delays to a network model that is not actually modeling routers, credits, or flits.
- Outcome: The reader knows when `SimpleNetwork` is sufficient and when Garnet is mandatory.

### Chapter 10: Garnet 3.0 Microarchitecture

> *When every flit, credit, and router pipeline stage matters, Garnet becomes the center of the story.*

- Core question: How does Garnet 3.0 implement router pipelines, NI behavior, virtual channels, and credit flow?
- Primary code anchors: `src/mem/ruby/network/garnet/README.txt`, `src/mem/ruby/network/garnet/GarnetNetwork.hh`, `src/mem/ruby/network/garnet/GarnetNetwork.cc`, `src/mem/ruby/network/garnet/NetworkInterface.hh`, `src/mem/ruby/network/garnet/NetworkInterface.cc`, `src/mem/ruby/network/garnet/Router.hh`, `src/mem/ruby/network/garnet/Router.cc`, `src/mem/ruby/network/garnet/InputUnit.hh`, `src/mem/ruby/network/garnet/InputUnit.cc`, `src/mem/ruby/network/garnet/OutputUnit.hh`, `src/mem/ruby/network/garnet/OutputUnit.cc`, `src/mem/ruby/network/garnet/SwitchAllocator.hh`, `src/mem/ruby/network/garnet/SwitchAllocator.cc`, `src/mem/ruby/network/garnet/CrossbarSwitch.hh`, `src/mem/ruby/network/garnet/CrossbarSwitch.cc`, `src/mem/ruby/network/garnet/NetworkLink.hh`, `src/mem/ruby/network/garnet/NetworkLink.cc`, `src/mem/ruby/network/garnet/NetworkBridge.hh`, `src/mem/ruby/network/garnet/NetworkBridge.cc`, `src/mem/ruby/network/garnet/flit.hh`, `src/mem/ruby/network/garnet/flit.cc`, `src/mem/ruby/network/garnet/Credit.hh`, `src/mem/ruby/network/garnet/Credit.cc`.
- Runnable anchors: Garnet-backed Ruby systems and focused synthetic traffic runs.
- Failure focus: VC starvation, credit deadlock, and topology-independent intuition that stops being valid once buffer depth and link width matter.
- Outcome: The reader can follow the code flow described in `README.txt` and map it to cycle-by-cycle NoC behavior.

### Chapter 11: Topologies, Routing, and Synthetic Traffic

> *A network is not one thing; it is a topology, a routing policy, a buffering policy, and a workload pattern interacting.*

- Core question: How do topology and routing choices appear in gem5, and how should we stress them?
- Primary code anchors: `configs/topologies/Crossbar.py`, `configs/topologies/Mesh_XY.py`, `configs/topologies/Mesh_westfirst.py`, `configs/topologies/MeshDirCorners_XY.py`, `configs/topologies/Cluster.py`, `configs/topologies/Pt2Pt.py`, `configs/topologies/CustomMesh.py`, `configs/example/noc_config/2x4.py`, `configs/example/garnet_synth_traffic.py`, `src/cpu/testers/garnet_synthetic_traffic/GarnetSyntheticTraffic.cc`, `src/cpu/testers/garnet_synthetic_traffic/GarnetSyntheticTraffic.hh`.
- Runnable anchors: `configs/example/garnet_synth_traffic.py` with injection-rate sweeps and topology changes.
- Failure focus: Studying only application workloads prevents the reader from ever learning the raw network limit or the onset of saturation.
- Outcome: The reader can generate latency-throughput curves, explain routing choices, and diagnose network bottlenecks with purpose-built traffic.

---

## Part IV — CHI and Memory Controllers

### Chapter 12: CHI Protocol Structure in gem5

> *CHI is large enough that the only sane way to learn it is through the code organization itself.*

- Core question: How does gem5 decompose CHI into messages, cache behavior, memory behavior, and DVM support?
- Primary code anchors: `src/mem/ruby/protocol/chi/CHI.slicc`, `src/mem/ruby/protocol/chi/CHI-msg.sm`, `src/mem/ruby/protocol/chi/CHI-cache.sm`, `src/mem/ruby/protocol/chi/CHI-cache-ports.sm`, `src/mem/ruby/protocol/chi/CHI-cache-actions.sm`, `src/mem/ruby/protocol/chi/CHI-cache-transitions.sm`, `src/mem/ruby/protocol/chi/CHI-cache-funcs.sm`, `src/mem/ruby/protocol/chi/CHI-mem.sm`, `src/mem/ruby/protocol/chi/CHI-dvm-misc-node.sm`, `src/mem/ruby/protocol/chi/CHI-dvm-misc-node-ports.sm`, `src/mem/ruby/protocol/chi/CHI-dvm-misc-node-actions.sm`, `src/mem/ruby/protocol/chi/CHI-dvm-misc-node-transitions.sm`, `src/mem/ruby/protocol/chi/generic/CHIGenericController.hh`, `src/mem/ruby/protocol/chi/generic/CHIGenericController.cc`.
- Runnable anchors: Protocol inspection paired with short CHI runs from the modern stdlib path.
- Failure focus: Readers drown in CHI terminology unless the message channels and node roles are tied immediately to source files and concrete flows.
- Outcome: The reader can navigate the CHI protocol tree and explain where request, response, snoop, and data behavior lives.

### Chapter 13: Building CHI Systems in Legacy Ruby and the Stdlib

> *CHI in gem5 is not one API; it is a protocol implementation plus two real ways of building systems around it.*

- Core question: How do the legacy Ruby configuration path and the modern stdlib CHI path construct real CHI systems?
- Primary code anchors: `configs/ruby/CHI.py`, `configs/ruby/CHI_config.py`, `configs/example/noc_config/2x4.py`, `tests/gem5/chi_protocol/configs/chi-with-isa.py`, `tests/gem5/chi_protocol/test_chi_per_isa.py`, `tests/gem5/chi_protocol/README.md`, `src/python/gem5/components/cachehierarchies/chi/private_l1_cache_hierarchy.py`, `src/python/gem5/components/cachehierarchies/chi/private_l1_private_l2_cache_hierarchy.py`, `src/python/gem5/components/cachehierarchies/chi/nodes/*`.
- Runnable anchors: `tests/gem5/chi_protocol/configs/chi-with-isa.py` on RISC-V as the main path, with Arm and x86 used as reality anchors because the test already exercises all three.
- Failure focus: Treating CHI as “Arm-only” or as “just another MESI variant” obscures the actual architectural and configuration differences.
- Outcome: The reader can build, read, and reason about both a legacy CHI configuration and a modern stdlib CHI hierarchy.

### Chapter 14: DRAM Controllers, Address Mapping, and the Off-Chip Bottleneck

> *A cache miss is not done when it leaves the chip; it enters another queueing system with its own state machine and locality rules.*

- Core question: How do gem5 memory controllers schedule commands, model bank state, and map addresses across channels?
- Primary code anchors: `src/mem/mem_ctrl.hh`, `src/mem/mem_ctrl.cc`, `src/mem/dram_interface.hh`, `src/mem/dram_interface.cc`, `src/mem/mem_interface.hh`, `src/mem/mem_interface.cc`, `src/mem/AddrMapper.py`, `src/python/gem5/components/memory/*`, `src/python/gem5/components/memory/dram_interfaces/*`, `configs/example/gem5_library/memory_traffic.py`.
- Runnable anchors: Traffic-generator sweeps across DDR, LPDDR, and HBM-backed stdlib memories.
- Failure focus: Interpreting memory latency without checking interleaving, row locality, and bank conflicts is one of the fastest ways to misread a gem5 result.
- Outcome: The reader can explain where DRAM latency comes from in gem5 and design experiments that isolate mapping and controller effects.

### Chapter 15: Optional Advanced Backends: HBM, NVM, DRAMSys, and HMC

> *The core book should master gem5's built-in path first, then widen the design space deliberately.*

- Core question: What changes when gem5 models more exotic or externally integrated memory systems?
- Primary code anchors: `src/mem/hbm_ctrl.hh`, `src/mem/hbm_ctrl.cc`, `src/mem/nvm_interface.hh`, `src/mem/nvm_interface.cc`, `src/mem/hetero_mem_ctrl.hh`, `src/mem/hetero_mem_ctrl.cc`, `src/mem/dramsys.hh`, `src/mem/dramsys.cc`, `src/mem/dramsys_wrapper.hh`, `src/mem/dramsys_wrapper.cc`, `src/mem/dramsim3.hh`, `src/mem/dramsim3.cc`, `configs/example/dramsys.py`, `configs/example/hmctest.py`, `configs/example/hmc_hello.py`.
- Runnable anchors: Small optional labs using the example scripts above.
- Failure focus: Pulling external-memory complexity into the main narrative too early makes the book broader but weaker.
- Outcome: The reader understands what these backends add, when they are worth the complexity, and why they remain optional in the core learning path.

---

## Part V — Integration and Extension

### Chapter 16: End-to-End Performance Analysis and Debugging

> *A simulator only becomes useful when the reader can localize a bottleneck instead of just observing one.*

- Core question: How do we isolate cache, protocol, network, and DRAM bottlenecks in one experiment?
- Primary code anchors: `src/mem/ruby/profiler/*`, `src/mem/probes/*`, `tests/gem5/stats/README.md`, `configs/example/ruby_random_test.py`, `configs/example/garnet_synth_traffic.py`, `tests/gem5/traffic_gen/configs/simple_traffic_run.py`, `tests/gem5/chi_protocol/configs/chi-with-isa.py`.
- Runnable anchors: A sequence of increasingly rich experiments that reuse the harnesses introduced earlier instead of inventing new ones.
- Failure focus: The wrong layer gets blamed whenever the experiment does not isolate components cleanly.
- Protocol-level debugging section: cover `DPRINTF(RubySlicc, ...)` for runtime trace output from SLICC code, `APPEND_TRANSITION_COMMENT(...)` for annotating the protocol trace with per-transition debugging information, and the `RubySlicc` debug flag. These are the three primary tools for diagnosing protocol-level issues and complement the profiler-based analysis.
- Outcome: The reader can answer “where did the cycles go?” with a defensible methodology rather than a guess.

### Chapter 17: Final Project: Extend a Protocol or Network Model

> *Mastery means changing the model responsibly, not just reading it.*

- Core question: How does a reader go from understanding a gem5 memory model to extending one and validating the result?
- Primary code anchors: `src/mem/slicc/*`, `src/mem/ruby/protocol/SConscript`, `src/mem/ruby/protocol/Kconfig`, `src/mem/ruby/protocol/MI_example*`, `configs/learning_gem5/part3/*`, `configs/example/ruby_random_test.py`, `src/cpu/testers/rubytest/*`, `configs/topologies/CustomMesh.py`, `src/mem/ruby/network/garnet/RoutingUnit.cc`.
- Runnable anchors: A protocol extension path validated with `RubyTester`, and an optional NoC extension path validated with `GarnetSyntheticTraffic`.
- Failure focus: Adding states, messages, or routing behavior without a verification and measurement story.
- Outcome: The reader finishes the book by implementing and evaluating one meaningful extension to gem5's memory or NoC stack.

---

## Appendices

### Appendix A: Build and Debug Workflow

- Anchors: `SConstruct`, `README.md`, `docs/README`, `src/mem/ruby/protocol/Kconfig`.
- Focus: build variants, protocol-enabled builds, and the minimum debug workflow needed for the labs.
- Protocol build integration: `.slicc` manifest file syntax and file ordering requirements (types must be declared before usage), Kconfig protocol registration for new protocols, the one-protocol-per-build constraint (gem5 compiles only one coherence protocol at a time — switching protocols requires a separate build directory), and SLICC HTML documentation generation (`SLICC_HTML` build option).

### Appendix B: Memory-System Code Atlas

- Anchors: `src/mem/cache/`, `src/mem/ruby/system/`, `src/mem/ruby/protocol/`, `src/mem/ruby/network/`, `src/mem/`, `src/python/gem5/components/cachehierarchies/`, `src/python/gem5/components/memory/`.
- Focus: a directory-level map so readers can quickly locate the right subsystem during later chapters.

### Appendix C: SLICC Quick Reference

- Anchors: `src/mem/slicc/*`, `src/mem/ruby/protocol/RubySlicc_*.sm`.
- Focus: syntax, generated concepts, common patterns, and common mistakes.
- Known gotchas catalogue: `mandatoryQueue` hard-coded name, SLICC name-mangling (`TBE` → `L1Cache_TBE`) requiring `template` syntax, mandatory `setMRU()` on every cache hit/fill, `dequeue()` delayed one cycle, `MessageSize` required in every message type, SLICC error line numbers pointing after the actual error.
- Functional access contract: signatures and semantics for `getAccessPermission()`, `setAccessPermission()`, `functionalRead()`, `functionalWrite()`, and the `AccessPermission` enum values.
- Standard types: `NetDest` bitvector (sharers/owner tracking) with `add`, `remove`, `clear`, `count`, `isElement` operations; `TBETable` with template syntax; `DirectoryMemory` with lazy allocation.
- Buffer idioms: `enqueue(buffer, MsgType, latency)` with implicit `out_msg`, `peek(buffer, MsgType)` with implicit `in_msg`, `dequeue(clockEdge())`, `isReady(clockEdge())`, `stall()` / `recycle()` / `stall_and_wait(address)` with tradeoffs.
- Debugging idioms: `DPRINTF(RubySlicc, ...)`, `APPEND_TRANSITION_COMMENT(...)`, SLICC HTML generation.

### Appendix D: Garnet Parameter and Topology Reference

- Anchors: `configs/network/Network.py`, `configs/topologies/*.py`, `src/mem/ruby/network/garnet/README.txt`.
- Focus: router latency, link width, VC counts, routing, and topology construction.

### Appendix E: DRAM Parameter and Memory-Model Reference

- Anchors: `src/python/gem5/components/memory/dram_interfaces/*`, `src/mem/dram_interface.hh`, `src/mem/dram_interface.cc`.
- Focus: how gem5 parameters map to memory-model concepts and timing terms.

### Appendix F: Glossary and Notation

- Focus: one term per concept, one notation table for the whole book, and explicit mappings from prose to gem5 class and file names.
