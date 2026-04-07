# Cache Subsystem Parameters

A structured taxonomy of parameters in a modern cache subsystem, covering CPU/SoC designs with multi-level caches and distributed LLCs.

---

# Part I — Storage Organization

## 1. Capacity & Geometry

- **Cache size** — total data capacity per level (e.g., 32 KiB L1, 256 KiB L2, 8 MiB LLC)
- **Line size** — granularity of storage and transfer (e.g., 64 B, 128 B); also called block size
- **Associativity** — number of ways per set; trades conflict misses against lookup energy
- **Number of sets** — derived: `sets = size / (line_size × ways)`

## 2. Tag Organization

- **Tag bits** — upper address bits stored per line to identify the block
- **Tag array structure** — separate tag SRAM vs unified tag+data
- **Sector cache** — one tag covers multiple contiguous sub-blocks, each with a valid bit; reduces tag overhead
- **Sub-block (sub-line) validity** — per-word or per-sub-block valid/dirty bits within a line
- **Tag compression** — compress tag storage to increase effective associativity (e.g., partial tags, way-halving)
- **Tag-only operations** — probe tag array without reading data (used for snoop responses, coherence queries)

## 3. Indexing & Address Mapping

- **Modular indexing** — standard: low-order bits select the set
- **XOR / hash indexing** — XOR high-order bits into index to reduce conflict misses
- **Skewed associativity** — different hash function per way; spreads conflicts more evenly than set-associative
- **Z-cache indexing** — generalized skewed scheme allowing variable-distance replacement
- **Page coloring interactions** — OS page placement can alias with cache indexing, creating hot sets
- **VIPT / PIPT / VIVT** — whether index and tag use virtual or physical addresses; affects aliasing and TLB timing

## 4. Banking & Slicing

### Banking
- **Number of banks** — independent SRAM arrays accessed in parallel
- **Bank interleaving** — which address bits select the bank (line-interleaved, set-interleaved)
- **Conflict behavior** — bank conflicts add queuing delay; multi-port vs banked single-port trade-offs

### Distributed LLC
- **Number of slices** — typically one per core or per cluster
- **Slice mapping function** — hash, XOR, or address-bit-based routing to the home slice
- **NUCA vs UCA** — Non-Uniform Cache Architecture (latency varies by slice distance) vs Uniform

---

# Part II — Data-Flow Policies

## 5. Write Policies

### Allocation
- **Write-allocate** — fetch the line on a write miss, then modify in cache
- **No-write-allocate** — write directly to the next level without allocating

### Propagation
- **Write-back** — defer writes to next level until eviction; reduces bandwidth
- **Write-through** — every write immediately propagated; simplifies coherence

### Write handling
- **Write combining** — merge multiple writes to the same line before sending downstream
- **Write coalescing** — detect streaming write patterns and delay/batch writes for full-line merging
- **Write-no-allocate transition** — dynamically switch from write-allocate to write-no-allocate when consecutive full-line writes are detected (e.g., `memset`)
- **Write buffers** — size, merge capability, drain policy

## 6. Inclusion, Fill & Data Movement

### Inclusion policy
- **Inclusive** — superset property: every line in L1 is also in L2; simplifies snooping
- **Exclusive** — no duplication: a line lives in exactly one level; maximizes effective capacity
- **Non-inclusive (NINE)** — no enforcement either way; lines may or may not be duplicated

### Fill & bypass
- **Read miss allocation** — always allocate vs selectively bypass (e.g., for streaming reads)
- **Write miss allocation** — allocate vs bypass based on pattern detection
- **Streaming/bypass detection** — identify non-reuse access patterns to avoid cache pollution
- **Insertion policy on fill** — where in the replacement order a new line is inserted (MRU, LRU, bimodal)

### Data movement
- **Cache-to-cache transfers** — supply data from a peer cache instead of main memory
- **Intervention** — a cache directly forwards data to the requester in response to a snoop
- **Silent eviction** — drop clean lines without notifying next level (vs explicit clean writeback)
- **Back-invalidation** — inclusive LLC invalidates L1/L2 copies when evicting a line
- **Victim promotion / demotion** — rules for moving lines between levels on eviction

---

# Part III — Access & Concurrency

## 7. Latency & Pipelining

- **Hit latency** — cycles from request to data delivery on a hit
- **Miss penalty** — additional cycles for next-level access on a miss
- **Pipeline stages** — tag lookup → data read → response; determines throughput
- **Tag/data access mode:**
    - **Parallel** — tag and data read simultaneously; lower latency, higher energy
    - **Sequential** — data read only after tag hit confirmed; lower energy, higher latency
- **Critical word first** — deliver the requested word immediately, fill remaining line in background
- **Early restart** — resume execution as soon as the critical word arrives (before full line fill)
- **Way prediction** — predict the matching way to read only one data bank, reducing energy

## 8. Miss Handling & MLP

### MSHRs (Miss Status Holding Registers)
- **Number of entries** — determines maximum outstanding misses (memory-level parallelism)
- **Per-set vs global** — whether MSHRs are partitioned per set or shared globally
- **Target merging** — coalesce multiple requests to the same line into one MSHR entry
- **Targets per MSHR** — maximum requests that can merge into a single outstanding miss
- **Allocation policy** — demand vs prefetch priority for MSHR slots
- **Demand reserve** — reserve a minimum number of MSHRs for demand requests, preventing prefetch starvation

### Fill / writeback buffers
- **Number of entries** — separate queue for returning fills or outgoing writebacks
- **Partial line handling** — support for sub-line fills or writes

### Concurrency
- **Hit-under-miss** — continue serving hits while a miss is outstanding
- **Miss-under-miss** — allow multiple concurrent misses (requires multiple MSHRs)
- **Max outstanding misses** — hard limit on concurrent next-level requests

## 9. Bandwidth & Concurrency

- **Read ports / write ports** — number of independent access ports per bank
- **Max accesses per cycle** — aggregate throughput limit
- **Internal crossbar bandwidth** — interconnect bandwidth within a multi-banked cache
- **Arbitration policy** — priority scheme when multiple requestors compete (round-robin, fixed priority, age-based)

---

# Part IV — Replacement & Eviction

## 10. Replacement Policies

### Core policies
- **LRU** — evict least recently used; optimal for stack-like access patterns
- **Pseudo-LRU (PLRU)** — tree-based or bit-based approximation of LRU; lower hardware cost
- **Random** — uniform random victim selection; simple, avoids pathological patterns
- **FIFO** — evict oldest-inserted block regardless of access recency
- **MRU** — evict most recently used; useful for cyclic/scanning patterns
- **NRU** — Not Recently Used; 1-bit approximation of LRU
- **LFU** — evict least frequently used; good for stable working sets

### Advanced policies
- **RRIP** — Re-Reference Interval Prediction; assigns re-reference priority values
- **BRRIP** — Bimodal RRIP; inserts most blocks with long re-reference interval
- **DRRIP** — Dynamic RRIP; set-dueling between BRRIP and SRRIP
- **BIP / DIP** — Bimodal / Dynamic Insertion Policy; controls MRU vs LRU insertion position
- **SHiP** — Signature-based Hit Predictor; uses PC or memory-address signatures to predict reuse
- **Hawkeye** — uses Belady's optimal algorithm on past accesses to train an insertion predictor
- **Mockingjay** — extends Hawkeye with more precise eviction-time estimates
- **Second Chance** — FIFO with reference-bit reprieve; accessed pages get a second pass

### Supporting parameters
- **Insertion position** — MRU end, LRU end, or bimodal (probabilistic)
- **Confidence / saturating counters** — bits per entry for re-reference prediction
- **Set dueling** — dedicating a subset of sets to each candidate policy to choose the winner dynamically
- **Dueling constituency size** — how many sets form one sampling region

---

# Part V — Prefetching

## 11. Prefetching

### Prefetcher types
- **Next-line** — prefetch the adjacent cache line
- **Tagged** — prefetch next N lines on any access (degree-controlled)
- **Stride** — detect fixed-stride patterns from a PC-indexed table
- **Stream** — detect sequential streams and prefetch ahead
- **Spatial (SMS, STeMS)** — learn spatial access patterns within a memory region
- **Best Offset (BOP)** — evaluate candidate offsets to find the best prefetch distance
- **Signature-based (SPP, SPP-V2)** — compress access patterns into signatures to predict future accesses
- **Delta-correlating (DCPT)** — correlate sequences of address deltas to predict future deltas
- **AMPM** — Access Map Pattern Matching; maps access patterns within hot zones
- **Temporal / history-based (ISB, STMS)** — replay previously observed address sequences
- **Indirect** — follow pointer chains or indexed data structures
- **Composite / multi-prefetcher** — combine multiple prefetcher strategies

### Control parameters
- **Degree** — number of prefetches generated per triggering access
- **Distance** — how far ahead of the demand stream to start prefetching
- **Confidence threshold** — minimum prediction confidence to issue a prefetch
- **Training window / table size** — capacity of the pattern history or training structures
- **Throttling** — dynamically adjust aggressiveness based on accuracy or bandwidth pressure
- **Queue size** — maximum outstanding prefetch requests

### Event filtering
- **Trigger on miss vs all accesses** — whether hits also train/trigger the prefetcher
- **Read / write filtering** — enable/disable for specific access types
- **Data / instruction filtering** — enable/disable for data vs instruction accesses
- **Prefetch-on-prefetch-hit** — re-trigger prefetching when a prefetched line is accessed

### Placement & pollution control
- **Target level** — which cache level receives prefetched lines (L1 / L2 / LLC)
- **Priority vs demand** — whether prefetches compete equally with demand requests
- **Insertion policy** — where prefetched lines are inserted in the replacement order
- **Accuracy tracking** — measure useful vs useless prefetches to guide throttling
- **Queue squash** — cancel queued prefetches when a demand access to the same address arrives
- **Cache snooping** — check if the line is already present before issuing the prefetch

---

# Part VI — Compression

## 12. Cache Compression

### Algorithm families
- **Base-Delta-Immediate (BDI)** — store a base value + small deltas per word; effective for pointer-heavy data
- **Frequent Pattern Compression (FPC)** — encode common patterns (zeros, repeated bytes, narrow values)
- **C-PACK** — dictionary-based pattern matching within a cache line
- **DISH** — deduplication and compression combined
- **Deduplication** — detect identical lines and share storage (one tag, one data copy)

### Parameters
- **Maximum compression ratio** — how many compressed blocks can share one tag entry (e.g., 2:1, 4:1)
- **Compression / decompression latency** — extra pipeline cycles for encoding/decoding
- **Compression throughput** — chunks processed per cycle
- **Size threshold** — minimum compression ratio to store compressed (vs uncompressed)
- **Co-allocation** — when a block compresses, try to share its data slot with a neighbor

### Interaction with tags
- Compressed caches typically use **sector-style tags**: one tag maps to a superblock, multiple compressed blocks can co-reside in one data slot
- Compression changes effective capacity dynamically — capacity depends on data content

---

# Part VII — Coherence

## 13. Coherence

### Protocol type
- MESI / MOESI / MESIF / CHI / ACE / TileLink

### Directory organization
- **Full-map directory** — one bit per sharer per line; precise but storage-heavy
- **Limited (compressed) directory** — bounded sharer list with overflow broadcast
- **Sparse directory** — directory entries allocated on demand; saves area for low-sharing workloads
- **Snoop filter** — tracks which caches may hold a line to suppress unnecessary snoops

### Sharer tracking
- Exact sharer bitvector vs approximate (coarse-grain, Bloom filter)
- Owner tracking (which cache has the dirty copy)

### Snoop behavior
- **Broadcast snooping** — all caches observe every coherence transaction; simple but bandwidth-heavy
- **Directory-based** — point-to-point messages guided by directory; scalable
- **Snoop filtering** — suppress snoops to caches known not to hold the line
- **Silent eviction handling** — whether evictions of clean shared lines require notification

### State extensions
- Owned (O), Forward (F) — additional stable states for data forwarding
- Transient states (e.g., IM, SM, IS) — in-flight states during state transitions

---

# Part VIII — System-Level Concerns

## 14. QoS & Partitioning

### Partitioning mechanisms
- **Way partitioning** — assign specific ways to specific requestors or partitions
- **Set partitioning** — assign cache sets to different requestors
- **Capacity partitioning** — soft cap: limit each partition to a fraction of total capacity
- **Utility-based partitioning (UCP)** — monitor utility (hits per way) and allocate ways to maximize total utility
- **UMON** — utility monitors that estimate hit curves per partition

### QoS controls
- QoS classes / priority levels
- Bandwidth throttling per partition
- Partition IDs — tagging requests with a partition identifier for enforcement

## 15. Virtual Memory Interaction

- TLB interaction — parallel vs serialized tag lookup relative to address translation
- Page size awareness — large pages change indexing and aliasing constraints
- Synonym handling — multiple virtual addresses mapping to same physical line

## 16. Power & Energy

- Way shutdown / drowsy ways — disable ways to reduce leakage
- Dynamic resizing — adapt effective capacity to workload phase
- Access gating — skip data read on miss (sequential access mode)
- Clock gating — gate cache bank clocks when idle
- Power-aware replacement — bias eviction toward ways that are candidates for shutdown

## 17. Reliability / ECC

- ECC per line (SECDED, etc.)
- Tag protection
- Data scrubbing — periodic background reads to detect and correct soft errors
- Retry mechanisms

## 18. Security

- Cache flushing behavior — flush timing and scope for context switches
- Partitioning for isolation — prevent cross-domain cache interference
- Randomized indexing — randomize set mapping to mitigate eviction-set construction
- Randomized replacement — prevent attacker from controlling eviction
- Side-channel mitigations — constant-time lookups, partition-based isolation

## 19. System / NoC Interaction

- LLC slice home mapping — how addresses route to LLC slices
- Request routing policy — how requests traverse the on-chip network
- Virtual channels for coherence traffic — separate VCs for requests, responses, snoops
- Credit-based flow control parameters
- Channel bandwidth balance — match cache port bandwidth to NoC link bandwidth
- Backpressure handling — behavior when downstream buffers are full

## 20. Debug / Observability

- Performance counters — hits, misses, MSHR utilization, prefetch accuracy, writeback counts
- Trace support — log individual cache events for offline analysis
- Warmup control — percentage of cache that must be populated before statistics collection begins
- Sampling mechanisms — statistical sampling to reduce trace overhead

---

# Part IX — Specialized Structures

## 21. Specialized Behaviors

- **Victim cache** — small fully-associative buffer that catches conflict-miss evictions (L0/L1.5)
- **Instruction vs data asymmetry** — I-cache is read-only, may have different size/associativity/latency
- **Hybrid write policies** — e.g., write-through L1 + write-back L2
- **Non-temporal accesses** — bypass cache for streaming data unlikely to be reused
- **Cache locking / pinning** — lock specific lines to prevent eviction (real-time, security)
- **Exclusive / victim LLC** — LLC acts as a victim cache, only holding evictions from upper levels
- **Prefetch buffers** — small dedicated buffer for prefetched lines, separate from main cache

---

# Summary

```
Cache subsystem =
    Storage    (size, associativity, tags, indexing, banks)
  + Data flow  (write policy, inclusion, fill/bypass)
  + Access     (latency, pipelining, MSHRs, bandwidth)
  + Eviction   (replacement policy, insertion control)
  + Speculation(prefetching)
  + Compression(algorithm, ratio, latency)
  + Coherence  (protocol, directory, snooping)
  + System     (QoS, partitioning, NoC, power, security)
```
