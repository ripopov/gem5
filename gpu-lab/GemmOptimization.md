# Persistent GEMM study on the eight-CU MI300X configuration

This study selects the GEMM shape that the naive, LDS and cache/register
samples now share, and evaluates a persistent, wave-specialised pipeline
against fairly tuned controls on the revised eight-CU MI300X configuration
at 2.1 GHz. The previous study at 128×256×128 predates that configuration;
its record is retained in
[results.json](workspace/experiments/gemm-study/results.json) and is not
compared with the numbers below.

**Selected shape: M=256, N=256, K=128** with 64×32 output tiles. The 32 tiles
are walked by eight persistent workgroups, four tiles each, verified from the
kernel's own tile counters on every run. The naive sample takes 107 s of
launcher wall time here, alone on the host. The delivered `hip-matmul-lds` kernel streams
K-stages through a four-slot LDS ring with two producer waves feeding two
compute groups that alternate output tiles and store their own results.

## Result summary

| Kernel | Organisation | Workgroups × tiles | GPU cycles, median | Min–max | Wall, median |
| --- | --- | ---: | ---: | ---: | ---: |
| Naive `hip-matmul` | one output per thread, 256 workgroups | 256 × 1 | 672,600 | one run | 107.3 s |
| Nonpersistent LDS, single buffer | cooperative loads, `s_barrier` | 32 × 1 | 322,109 | 320,341–323,941 | 29.0 s |
| Nonpersistent LDS, double buffer | cooperative loads, `s_barrier` | 32 × 1 | 321,898 | 320,070–323,023 | 27.9 s |
| Cooperative persistent LDS, double buffer | all waves load and compute, `s_barrier` | 8 × 4 | 317,488 | 315,528–319,562 | 24.6 s |
| Cooperative persistent LDS, single buffer | all waves load and compute, `s_barrier` | 8 × 4 | 317,987 | 315,958–319,373 | 24.6 s |
| Cooperative persistent LDS, single buffer | 16 workgroups | 16 × 2 | 321,368 | 319,522–326,129 | 25.2 s |
| Specialised, two groups, 4-slot ring (**delivered**) | 2 producer waves, 2×2 compute waves | 8 × 4 | 300,264 | 299,156–304,880 | 27.3 s |
| Specialised, two groups, 2-slot ring | 2 producer waves, 2×2 compute waves | 8 × 4 | 300,597 | 299,827–302,537 | 27.6 s |
| Specialised, two groups, 6-slot ring, 2 prefetched stages | 2 producer waves, 2×2 compute waves | 8 × 4 | 303,777 | 301,626–309,958 | 28.2 s |
| Specialised, one group, 4-slot ring, 2 prefetched stages | 2 producer waves, 2 compute waves | 8 × 4 | 311,386 | 310,288–324,984 | 26.8 s |
| Specialised, two groups, 4-slot ring | as delivered, 16 workgroups | 16 × 2 | 285,858 | 282,777–294,184 | 27.1 s |
| Specialised, one group, 4-slot ring, 2 prefetched stages | 16 workgroups | 16 × 2 | 295,466 | 284,866–303,409 | 26.3 s |
| Cache/register only, persistent | 32×32 wave tiles, register prefetch, no LDS | 8 × 4 | 327,086 | 324,581–327,651 | 26.2 s |
| Cache/register only, nonpersistent | 32×32 wave tiles, register prefetch, no LDS | 32 × 1 | 594,309 | 593,241–609,985 | 33.8 s |

Cycles are GPU-active cycles at the 2.1 GHz shader clock (476 ticks each),
medians of five fresh simulator runs per variant launched one at a time; the
naive sample is one run. Ranges are min–max. Wall time is the launcher's
duration excluding compilation.

The delivered specialised pipeline is **5.4% faster** than the strongest
nonspecialised LDS control, the cooperative persistent double buffer, with
nonoverlapping five-run ranges (299,156–304,880 against 315,528–319,562), and
6.7% faster than the best conventional one-tile-per-workgroup LDS kernel. It
is also 8.2% faster than the strongest cache/register-only kernel, which
never uses LDS and whose nonpersistent form suffers from L1 thrashing once all
32 of its workgroups become resident. Running the same specialised kernel with
sixteen workgroups is faster still (285,858 cycles), but each workgroup then
processes only two tiles, so it is reported and not delivered. Ring depth
beyond four slots, a second prefetched stage and a single compute group all
measured slower at eight workgroups; the one-group variants show the cost of
serialising each tile's stores behind its computation. The naive sample's
107 s is below the two-to-three-minute target; every larger shape that kept
four tiles per workgroup either exceeded three minutes or removed the
specialisation's lead (see [Shape selection](#shape-selection)).

The lead is small in absolute terms because the platform is memory-bound;
[Why the margins are small](#why-the-margins-are-small) quantifies this. The
wall-time column shows the same ordering but compresses the differences,
because launcher time includes container start-up, input copies and host
verification that are identical for all kernels.

## Measurement contract

All comparisons use the `gem5-kmd` backend, the audited MI300X per-CU
resources, eight CUs, 1 MiB L2, the single 16 GB/s HBM interface and the
2.1 GHz shader clock. Inputs, FP32 multiply–accumulate arithmetic and the
full-output comparison against the host reference are identical for every
kernel; the checksum at the selected shape is `102760448`.

Two departures from the audit images are deliberate and recorded in every
result record:

- The simulator binary carries two generic GPU-model corrections that the
  eight-and-more-wave workgroups of this study require (see
  [Model findings](#model-findings)). It is layered onto the validated
  `mi300x-8cu` runner and full-system images as
  `gem5-gpu-hsakmt-runner:model-fixes` and `gem5-gpufs-runtime:model-fixes`,
  with the binary hash in the image labels.
- Every timed run passes `--reg-alloc-policy=dynamic`. At the time of the
  study the lab default was the simple register allocator, which admits a
  workgroup only to SIMDs that hold no other waves and did not check
  capacity, so it either serialised workgroups on a CU or piled a whole
  workgroup onto one SIMD, and it asserted once three 129-register
  workgroups shared a CU. That assertion killed the double-buffered
  nonpersistent control and every persistent variant with more than two
  workgroups per CU. The dynamic allocator, already validated by the
  configuration audit, checks real capacity and spreads waves across SIMDs;
  it has since become the lab default and the simple allocator's capacity
  check was fixed. Exploration results taken under the simple allocator are kept in
  [exploration-simple.csv](workspace/experiments/gemm-persistent/exploration-simple.csv)
  for reference only.

The persistent kernels copy their tile counters back to the host, which the
runtime performs with one small blit kernel. It runs after the GEMM and adds
well under a thousand GPU-active cycles; the verifier expects two modeled
kernels for the two optimized samples and one for the naive sample.

## Shape selection

The naive sample's wall time scales with M·N·K. The candidates below were
measured once each on this host, several concurrently, so their wall times
are indicative; the selected shape was remeasured alone in the result summary.

| Shape | Naive wall | Naive GPU cycles | 64×64 tiles | 64×32 tiles |
| --- | ---: | ---: | ---: | ---: |
| 128×256×128 (previous) | 54 s | 406,612 | 8 | 16 |
| 256×256×128 | 103 s | 710,012 | 16 | 32 |
| 256×512×64 | 118 s | 780,976 | 32 | 64 |
| 256×256×160 | 118 s | 873,677 | 16 | 32 |
| 256×256×192 | 159 s | 1,036,571 | 16 | 32 |
| 256×512×96 | 162 s | 1,084,422 | 32 | 64 |
| 256×384×128 | 171 s | 1,029,458 | 24 | 48 |
| 256×512×128 | 200 s | 1,402,204 | 32 | 64 |

Four tiles per persistent workgroup on eight CUs needs 32 tiles with eight
workgroups or 64 tiles with sixteen. Every shape in the table was explored
with the controls and the specialised pipeline; the single-run exploration is
in [exploration-dynamic.csv](workspace/experiments/gemm-persistent/exploration-dynamic.csv).
The specialised pipeline led the nonspecialised LDS controls only at
256×256×128 with 64×32 tiles and at 256×256×192 with the same tiles, and in
both cases the lead was largest with sixteen resident workgroups, which gives
each workgroup two tiles. With eight workgroups and four tiles each the lead
shrinks, and at 256×512×128, where sixteen workgroups also get four tiles,
it disappears. Larger tile counts per CU, not more tiles per workgroup, are
what the pipeline converts into speed on this platform, because the
workload is memory-bound (see [Why the margins are small](#why-the-margins-are-small)).
256×256×128 was selected because it is the only shape that satisfies the
tile-count requirement with a measurable lead, and its naive run is closest
to the two-minute budget.

## The persistent pipeline

All persistent kernels live in
[`matmul_persistent.hh`](workspace/samples/common/matmul_persistent.hh) and
share the MFMA mapping, tile loaders and host contract of the nonpersistent
kernel in `matmul_mfma.hh`. Each workgroup receives a contiguous run of output
tiles in row-major tile order, so consecutive tiles reuse the same A rows.

In the specialised organisation, `PRODUCERS` threads form producer waves that
walk the (tile, K-stage) sequence, keep `PREFETCH` stages of global loads in
flight, and write each stage into a `STAGES`-slot LDS ring. `GROUPS` compute
groups take alternate output tiles: group g consumes the stages of tiles
g, g+GROUPS, ..., keeps its accumulators in AGPRs, and stores its own tile
while the other group computes. Waves never use `s_barrier` after the ring
counters are initialised. Instead each slot carries two monotonically
increasing LDS counters, `full` and `empty`
([`wg_pipeline.hh`](workspace/samples/common/wg_pipeline.hh)): producers add
one after writing a slot, consumers add one after reading it, and each side
waits for the count that its fill number implies. A workgroup-scope release
fence (`s_waitcnt lgkmcnt(0)`) orders the data accesses before the counter
update, an acquire fence orders later accesses behind the observed value, and
LDS requests from one CU are serviced in issue order, so the protocol needs
only `ds_read_b32`, `ds_add_u32`, `s_waitcnt` and `s_sleep`, all implemented
by the simulator. `DS_GWS_BARRIER` is unimplemented and `s_barrier` is
workgroup-wide, so neither could separate the roles.

The protocol was validated before any GEMM measurement with
[`sync-test.cpp`](workspace/experiments/gemm-persistent/sync-test.cpp):
pattern-checked payloads, per-stage pseudo-random `s_sleep` delays on both
sides, one or two producer waves feeding one or two consumer groups through
rings of 2, 3, 4 and 6 slots, 96 stages per workgroup (16 to 48 reuses of
each ring) on eight workgroups. Every variant passes on simdojo and on
gem5-kmd, and removing the consumer waits (`BROKEN=1`) fails with millions
of mismatches on both, so the check detects violations. Run ids are in
[sync-validation.json](workspace/experiments/gemm-persistent/sync-validation.json).

The cooperative persistent organisation (`PRODUCERS=0`) keeps the same tile
walk but lets every wave load and compute, separated by `s_barrier` with one
or two LDS buffers. The cache/register organisation (`USE_LDS=0`) lets each
wave load its own fragments and prefetch the next stage across tile
boundaries. `WORKGROUPS` sets the persistent grid; `ORDER=1` switches to a
strided tile walk; `SPIN_SLEEP` sets the polling interval.

## Overlap evidence

A `GPUTrace,GPUMem` trace of the delivered configuration (eight workgroups,
four tiles each; run
`gemm-trace-spec-p128-g2-s4-f1-w8-256-256-128-1789507524446002212`) was
analysed with `overlap.py`, which classifies waves by behaviour (MFMA issuers
are compute waves, load-only waves are producers) and tracks every global
load and store from issue to its completion message. All 1,536 loads and
1,040 stores matched a completion. Of the 8,192 MFMA instructions:

| MFMAs issued while ... | Count | Share |
| --- | ---: | ---: |
| a global store of the *other* compute group in the same workgroup was pending | 1,199 | 14.6% |
| a producer wave's global load was pending | 7,033 | 85.8% |
| a store of the same wave was pending | 0 | 0% |

The first row is the handoff the design targets: one group's output stores
for tile *t* drain while the other group is already multiplying tile *t+1*.
Every CU shows it (128 to 160 MFMAs per CU, i.e. during three of the four
tile boundaries the store of the previous tile is still in flight). The
second row shows the producer running ahead across stage and tile boundaries
without barriers. The zero third row reflects the design: compute waves never
wait on `vmcnt`, so their own stores never gate them, and the next tile's
first MFMA is reached while the previous tile's stores are pending in
another group, never in the same wave. Per-CU MFMA spans are balanced within
1.5% (220,892 to 224,082 cycles), confirming the four-tile-per-workgroup
distribution that the kernel's own counters report (`tiles_per_workgroup=4`
for all eight workgroups on every run).

Load latency in this trace has a median of 1,678 cycles (p90 9,184, maximum
42,860) and store completion a median of 2,205 cycles (p90 14,527), against
512 cycles of MFMA work per K-stage per wave; see
[final-latency.txt](workspace/experiments/gemm-persistent/final-latency.txt)
and [final-overlap.json](workspace/experiments/gemm-persistent/final-overlap.json).
The saved ISA shows the same structure statically: producers issue
`global_load_dwordx4` for the next stage before waiting on the current one
(`s_waitcnt vmcnt(N)` with N counting the younger loads), write LDS, then
`ds_add_u32` the slot's `full` counter; consumers poll `ds_read_b32` with
`s_sleep 1`, read fragments with `lgkmcnt` waits interleaved with MFMAs, add
to `empty`, and issue their 16 `global_store_dword` per tile without any
`vmcnt` wait before starting the next tile.

## Controls and tuning

The controls were tuned at every shape under the same allocator with the
sweeps recorded in the exploration CSV; the finalists above are the best of
each family at the selected shape.

- *Nonpersistent tiled GEMM* (`PERSISTENT=0`): single- and double-buffered
  LDS with two-fragment LDS prefetch and padded layouts, the previous study's
  finalist family. With 32 workgroups the dispatcher keeps three to four of
  them resident per CU.
- *Cooperative persistent GEMM*: one or two LDS buffers, eight or sixteen
  workgroups, contiguous or strided tile order, mid- or late publication.
- *Specialised pipeline*: 64, 128 and 256 producer threads, one to three
  compute groups, rings of 2 to 6 slots, one to four prefetched stages, 8,
  16 and 24 workgroups, polling intervals from continuous to 512 cycles, and
  one-fragment or two-fragment LDS prefetch. Deeper rings and prefetch did
  not help beyond four slots and one prefetched stage; polling interval
  changes stayed within run-to-run variation.
- *Cache/register only*: 32×32 wave tiles with and without register prefetch,
  nonpersistent and persistent. Under the dynamic allocator the nonpersistent
  kernel's 32 four-wave workgroups all become resident and thrash the 32 KiB
  vector L1, so the persistent variant with eight workgroups is the strong
  control. The persistent variant with 64×64 tiles at 16 or 24 workgroups
  aborted the simulator in the GPU TLB during the study; the cause was a
  generic ISA-model defect described under [Model findings](#model-findings),
  fixed after the measurements, and that variant is excluded from the tables.

Resource use is recorded per variant (`resources` in each result record).
The delivered specialised kernel reports 75 VGPRs and 41 SGPRs in use, no
AGPRs (the compiler keeps the 64×16 accumulator in VGPRs on gfx942), no
scratch and no spills, 32,800 bytes of LDS for the four-slot ring and its
counters, one `s_barrier` (the counter initialisation), two `ds_add_u32`
signals, two `s_sleep` polling loops, 16 MFMAs per K-stage per wave and 16
output stores per wave per tile. Its descriptor requests 169 VGPRs because
LLVM rounds the allocation up to the occupancy that the LDS footprint
already implies (one workgroup per CU at eight workgroups). Launch-bounds
occupancy hints do not reduce the descriptor counts because LLVM sizes them
from the LDS-limited occupancy it already computes.

## Why the margins are small

Every kernel in this study is bound by the reduced memory system, not by
its instruction stream. At the selected shape the product needs
2·256·256·128 = 16.8 MFLOP. Eight CUs with four matrix cores each, one FP32
16×16×4 MFMA every 32 cycles, deliver 256 FLOP per CU-cycle, so the compute
floor is about 8,200 cycles. The minimum traffic is 512 KiB (A and B once,
C once): 69,000 cycles at the interface's 16 GB/s peak and about 178,000
cycles at the 6.2 GB/s the streaming probe of the eight-CU evaluation
achieved. Kernel time is therefore set by how continuously the memory system
is kept busy, and matrix-core utilisation is a few percent for every variant:
in the 256×512×64 counter comparison the matrix cores were busy for 305,000
wave-slot cycles across 32 SIMDs in a 344,000-cycle run.

The trace of a two-group specialised run at 256×512×64 measured the latency
that each design must hide: median load latency 2,972 cycles (p90 17,164),
median store completion 10,207 cycles (p90 32,039), both far beyond the 512
cycles of MFMA work in a K-stage. All kernels also read and write the same
number of DRAM bytes within run-to-run noise, so none of them changes the
traffic the memory system has to serve; the L2 absorbs the operand re-reads
for every organisation.

What the specialised pipeline changes is the shape of the request stream.
Its compute waves never wait on `vmcnt`, so their output stores never delay
the next tile's operand consumption, and the producer keeps one or two
stages of loads in flight without barrier stalls: barrier wait drops from
millions of wave-slot cycles to tens of thousands and wait-count stalls fall
by a quarter. It pays for this with spinning waves (tens of thousands of
extra LDS reads and scalar instructions) and with register pressure that
holds the whole workgroup's VGPR allocation at the producer's requirement,
which limits how many workgroups a CU can host. When a nonpersistent kernel
can keep three or four independent workgroups resident per CU, their natural
interleaving hides the same latency, and the two organisations end within a
few percent of each other. The specialised pipeline leads where the
nonpersistent kernel's residency is limited and the persistent kernel can
still place two workgroups per CU, as at 256×256×128; increasing K improves
neither the arithmetic intensity enough to matter (with roughly K/2 FLOP per
output byte, balance needs K above about 540 at the 16 GB/s peak and above
1,400 at the achieved 6.2 GB/s) nor the residency, which is why deeper K did
not create a larger margin.

These are measured behaviours of the reduced platform: one HBM interface,
one L2 controller and the audited request-path latencies. On hardware with
proportionally more bandwidth per CU the same kernels would be compute-bound
much earlier and the store/compute overlap would matter more; that is a
hypothesis this platform cannot test.

## Model findings

Preparing the study exposed five simulator behaviours. Four are fixed; the
fifth is documented and worked around.

1. `ComputeUnit::hasDispResources` compared a workgroup's total SGPR demand
   with one SIMD's 800 registers although waves are spread over four SIMDs,
   and LLVM rounds the descriptor SGPR count up to the architectural maximum
   whenever LDS limits occupancy. Any LDS kernel with eight or more waves per
   workgroup therefore panicked at dispatch. The check now uses the CU total,
   like the neighbouring VGPR check.
2. DS instructions issued with an empty EXEC mask returned early without
   releasing the wave's local-memory reservation, so a wave slot later
   panicked with a nonzero in-flight count. The compiler emits such
   instructions for short `if (threadIdx.x < N)` blocks without a skip
   branch. `Wavefront::releaseLmResource()` now balances the reservation, and
   the six LDS accounting unit tests still pass.
3. Global-memory atomics were applied twice on the gem5-kmd backend:
   [`atomic-probe.cpp`](workspace/experiments/gemm-persistent/atomic-probe.cpp)
   counted 8 where 4 was expected, with returned values following the doubled
   sequence, while the same binary was correct in full-system. The KMD
   launcher enables Ruby's `access_backing_store`, so that the attached
   process reads the same bytes through the shared VRAM backing store. With
   that option `RubyPort` re-executes every completed packet on physical
   memory, and for an atomic packet `AbstractMemory::access` performs the
   operation again after the VIPER directory has already performed it and
   written the result through. The coalescer now commits the protocol's
   result bytes for atomic packets itself and tells `RubyPort` not to apply
   the packet; the probe passes on both backends. The persistent kernels
   count tiles in registers regardless, which needs no atomics.
4. 16-bit vector ALU results were written into the low half of the VGPR
   only, leaving the upper 16 bits stale, whereas this ISA generation
   zero-extends them and the compiler relies on that: after an integer
   division's reciprocal temporary had used a register, a
   `v_lshrrev_b16` computing a row index left the reciprocal's upper bits in
   place, the row became 0xffe00000 and an operand load addressed memory
   512 MiB below matrix A. The page walk found no PTE and the TLB asserted.
   `VecOperand::write()` now zero-extends sub-dword results, and the walker
   reports a page fault with the faulting address instead of handing the TLB
   an invalid entry. The register-only persistent kernel with 64×64 tiles
   runs after the fix.
5. The simple register allocator behaviour described in the measurement
   contract. Its missing capacity check is fixed, so it now makes a
   workgroup wait for enough empty SIMDs instead of overflowing a pool, and
   the lab default is the dynamic allocator.

## Reproduction

From `gpu-lab/workspace`, with the images named above:

```sh
export HSAKMT_SDK_IMAGE=gem5-gpu-hsakmt-sdk:migration
export HSAKMT_RUNNER_IMAGE=gem5-gpu-hsakmt-runner:model-fixes
python3 experiments/gemm-persistent/finalize.py --shape 256 256 128 --repeat 5 --naive
python3 experiments/gemm-persistent/explore.py select --jobs 4 --config-arg=--reg-alloc-policy=dynamic
python3 experiments/gemm-persistent/overlap.py work/runs/RUN/gem5.log --compute-waves 2 --groups 2
```

`finalize.py` builds each finalist once and launches it five times serially,
then writes medians and ranges to `work/gemm-persist/final-summary.json`.
`explore.py` runs the named exploration study concurrently (its wall times
are not comparable) and `summary` prints what has completed. `overlap.py`
measures cross-tile overlap in a `GPUTrace,GPUMem` trace, `latency.py` the
load and store latency distribution, and `compare.py` the traffic and stall
counters of completed variants. `sync-test.cpp` and `atomic-probe.cpp` build
with `hipcc --offload-arch=gfx942 -O2 -std=c++17` in the SDK image. The
working notes in [NOTES.md](workspace/experiments/gemm-persistent/NOTES.md)
record the intermediate findings.

The integration gates are unchanged:

```sh
./scripts/run-all-samples.sh --backend simdojo
./scripts/run-all-samples.sh --backend gem5-kmd
FULL_SYSTEM_IMAGE=gem5-gpufs-runtime:model-fixes ./scripts/run-all-samples.sh --backend full-system
```
