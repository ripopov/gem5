# Ruby and Garnet Statistics Reference

This document is a source-checked reference for the Ruby and Garnet
statistics that matter in the ruby-book CHI mesh workloads.
It is designed as a lookup and diagnosis aid, not as a narrative chapter.
For each family below, this reference says where the stat comes from, what
exactly it measures, how it appears in `stats.txt`, and how to use it when
diagnosing a workload.

## Purpose And Scope

This reference includes stat families owned by Ruby or Garnet themselves.
That means it includes:

- Ruby profiler stats under `system.ruby.*`
- SLICC-generated controller aggregate stats such as
  `system.ruby.Cache_Controller.*`
- per-controller Ruby stats such as `*.TBEs.*`, `*.cache.*`,
  `*.outTransLatHist.*`, and message-buffer stats
- Garnet network, router, and link stats under `system.ruby.network.*`

This reference intentionally excludes generic inherited stats that happen to
live under the same subtree but are not Ruby- or Garnet-specific.
Examples of excluded noise are `power_state.pwrStateResidencyTicks::*` and
generic SimObject power-state fields.

## How These Stats Are Measured

Before looking at stat names, separate the main measurement patterns.
When gem5 dumps a stats block, it reports quantities accumulated since the
previous reset or dump for that block.
Ruby and Garnet do not measure everything in the same way.
Some stats are raw counters.
Some are time averages.
Some are latency sums that later get divided into averages.
Some are histograms over sampled events.

### Common Measurement Patterns

| Pattern | What the producer does | Example | Correct mental model |
|---|---|---|---|
| Raw event counter | Increment once when an event happens | `system.ruby.Cache_Controller.ReadShared::total`, `system.ruby.network.flits_received::total` | How many times something happened during the window |
| Time-average occupancy | Track occupancy over simulated time and divide the occupancy integral by the window length | `*.TBEs.avg_size`, `*.<buffer>.m_buf_msgs` | Average number of entries resident, not a final snapshot |
| Accumulated latency sum | Add one latency value when a sampled item completes | `system.ruby.network.flit_network_latency`, `system.ruby.network.packet_queueing_latency` | Sum of sampled delays, which must be divided by completions for a mean |
| Histogram sample set | Add one sample to a bucketed distribution | `system.ruby.m_latencyHistSeqr`, `*.inTransLatHist.Load` | Distribution over sampled items, not over time |
| Normalized activity rate | Divide raw activity by elapsed simulated cycles during stat collation | `system.ruby.network.avg_link_utilization`, `system.ruby.network.avg_vc_load` | Aggregate activity per cycle |

Most interpretation mistakes come from reading all of these as if they were
the same kind of average.
They are not.

### What The Sampled Object Actually Is

This document uses a few overloaded words that are easy to blur together.

- CPU-side Ruby request: one sequencer request that actually enters Ruby
- Coalesced follower: a later CPU access that reuses an already-outstanding
  Ruby request instead of creating a new one
- Controller event: one SLICC state-machine input processed by one controller
  instance
- Ruby message: one protocol message moving through a `MessageBuffer`
- Garnet packet: the network-level unit created from a Ruby message at the
  network interface
- Garnet flit: the flow-control unit used inside the network to carry a packet

That distinction matters because one user-visible memory operation can map to
several protocol events, one protocol message can become one packet, and one
packet can become several flits.

### What Latency Means In This Document

Latency stats in this document do not all measure the same path.

| Quantity | Start | End | Sampled once per |
|---|---|---|---|
| Sequencer request latency | request insertion into Ruby | sequencer completion | non-aliased Ruby request |
| Controller transition latency | recorded transition start | recorded transition completion | recorded controller event instance |
| Message delay | controller-specific message-delay observation point | controller-specific delay observation point | profiled message |
| Packet latency | packet lifetime as reconstructed at the NI | packet completion at the destination NI | received packet |
| Flit latency | flit lifetime as reconstructed at the NI | received flit completion point | received flit |

So when two stats both contain the word `latency`, they may still describe
different objects and different sampling points.

### What Utilization Means In This Document

Ruby and Garnet both use the word `utilization`, but they use it for different
things.

- Ruby occupancy-derived utilization, such as `*.TBEs.avg_util`, means
  time-average slots in use divided by configured capacity
- Message-buffer occupancy-derived utilization, such as `*.m_occupancy`, means
  time-average messages resident divided by maximum queue depth
- Garnet raw link-utilization stats such as `int_link_utilization` are not
  utilizations in the usual fraction-of-time-busy sense; they are raw flit
  counts
- Garnet `avg_link_utilization` is an aggregate flits-per-cycle rate summed
  over all flit-carrying links in the network
If one hundred links each carried `0.5` flits per cycle on average,
`avg_link_utilization` would report `50.0`, not `0.5`.
That is why Garnet's link-utilization numbers can be much larger than `1`.

## Quick Navigation

| Question | Best stat family |
|---|---|
| How many requests were in flight at issue time? | `system.ruby.m_outstandReqHistSeqr` |
| Which request types are slow? | `system.ruby.RequestType.<RubyRequestType>.*latency_hist_seqr` |
| Which controller object owns the queueing? | `*.<buffer>.m_buf_msgs`, `*.TBEs.avg_size` |
| Which HNF is hot? | `system.ruby.hnf*.cntrl.cache.m_demand_accesses` |
| Is the bottleneck in controller scheduling? | `*.fullyBusyCycles`, `*.delayHistogram` |
| Is the bottleneck in network transit or NI queueing? | `system.ruby.network.average_flit_vnet_latency`, `average_flit_vqueue_latency` |
| Which routers are hottest? | `system.ruby.network.routersXX.*activity` |
| Which physical links are hot? | `*.network_link*.flits_per_vnet::*` |
| Is traffic data-heavy or control-heavy? | `flits_received`, `packets_received`, `data_traffic_distribution`, `ctrl_traffic_distribution` |
| Is a protocol path retrying? | `*.inTransLatHist.<Event>.retries`, `*.outTransLatHist.<Event>.retries`, message-buffer `m_not_avail_count` |

## Minimal Analysis Set

If you only have time to inspect a small subset, start here.

### For Service-Point Hotspots

- `hnf*.cntrl.cache.m_demand_accesses`
- `hnf*.cntrl.TBEs.avg_size`
- `hnf*.cntrl.<buffer>.m_buf_msgs`
- `system.ruby.network.average_flit_network_latency`
- `system.ruby.network.average_flit_queueing_latency`

### For Network Bottlenecks

- `system.ruby.network.flits_received`
- `system.ruby.network.packets_received`
- `system.ruby.network.average_flit_vnet_latency`
- `system.ruby.network.average_flit_vqueue_latency`
- `system.ruby.network.average_packet_vnet_latency`
- `system.ruby.network.average_packet_vqueue_latency`
- `system.ruby.network.avg_link_utilization`
- `system.ruby.network.routersXX.crossbar_activity`
- `*.network_link*.flits_per_vnet::*`

### For Protocol-Path Debugging

- `system.ruby.Cache_Controller.<Event>::total`
- `system.ruby.Cache_Controller.<State>.<Event>::total`
- `*.inTransLatHist.<Event>`
- `*.outTransLatHist.<Event>`

## How To Read `stats.txt`

### Measurement Window Selection

Pick the stats block that matches the measurement window you actually want to
study.
The clean measurement pattern is:

- call `m5_reset_stats()` immediately before the region of interest
- run the region you want to measure
- call `m5_dump_reset_stats()` immediately after that region

`m5_reset_stats()` clears the accumulated counters so the next measurement
window starts from zero.
`m5_dump_reset_stats()` writes the current block to `stats.txt` and then
clears the counters again for the next phase.
If a benchmark repeats that pattern several times, `stats.txt` will contain
several measured blocks.
In that case, analyze the block that corresponds to the phase you care about.
Ignore teardown-only blocks unless teardown behaviour is the thing you are
studying.
Without explicit stats control, one dump can mix warmup, steady-state, and
teardown activity into the same block.

### Output Formats

Ruby and Garnet use three output styles.

#### Scalars

These are single values.
Examples:

- `system.ruby.network.average_flit_network_latency`
- `system.ruby.hnf15.cntrl.TBEs.avg_size`
- `system.ruby.network.routers05.crossbar_activity`

#### Vectors

These are one stat family split across indices.
They usually show one oneline row plus a `::total` line.
Examples:

- `system.ruby.network.flits_received`
- `system.ruby.network.avg_vc_load`
- `system.ruby.Cache_Controller.ReadShared`

Important detail:
For vectors emitted with `pdf | total | oneline`, the printed row is a
per-index breakdown and `::total` is the sum across indices.
For `system.ruby.Cache_Controller.<Event>`, the indices are controller
instances of that machine type.
For `system.ruby.network.flits_received`, the indices are vnets.

#### Histograms

Histograms emit a family of synthetic fields.
You will usually see:

- `::<bucket_size>`
- `::<max_bucket>`
- `::<samples>`
- `::<mean>`
- `::<gmean>`
- `::<stdev>`
- one unlabeled bucket row
- `::<total>`

For these histograms, `::total` is the total number of samples, not the sum
of latency values.
The `::mean` is the mean over samples recorded by the producer of that
histogram.
It is not automatically a time average.
That distinction matters a lot for `m_outstandReqHistSeqr`, which is sampled
at request insertion time in `Sequencer::insertRequest()`, not once per cycle.

### Zero-Suppressed Statistics

Many Ruby and Garnet stats use `statistics::nozero`.
If a stat is zero for the whole window, it may be omitted from `stats.txt`
entirely.
So absence often means zero, not missing instrumentation.
This is especially common for:

- message buffers that never carried traffic in that window
- transition histograms for unused events
- controller state or event vectors for impossible protocol paths

### CHI Virtual Network Map For These Runs

For the CHI protocol used here, the virtual-network assignment is defined
directly in the SLICC protocol files.
`src/mem/ruby/protocol/chi/CHI-cache.sm` and `CHI-mem.sm` state:

| Vnet | Meaning | Source proof |
|---|---|---|
| `0` | request | `virtual networks: 0=request` |
| `1` | snoop | `virtual networks: 1=snoop` |
| `2` | response | `virtual networks: 2=response` |
| `3` | data | `virtual networks: 3=data` |

They also mark only vnet `3` as `vnet_type="response"`, which Garnet treats
as the data vnet for traffic-distribution accounting.
That means:

- `system.ruby.network.data_traffic_distribution.*` counts only vnet `3`
  packets
- `system.ruby.network.ctrl_traffic_distribution.*` counts vnets `0`, `1`,
  and `2`

### Naming Patterns

Ruby and Garnet stats use a stable suffix vocabulary.
The object prefix tells you where the pressure lives.
The suffix tells you what kind of pressure it is.
Examples:

- `system.ruby.hnf15.cntrl.TBEs.avg_size`
  This is a time-average occupancy of HNF15's main TBE pool.
- `system.cpu0.l2.datOut.m_buf_msgs`
  This is a time-average queue depth of the `datOut` message buffer inside
  CPU0's Ruby L2 controller.
- `system.ruby.network.routers05.crossbar_activity`
  This is a count of crossbar traversals through Garnet router 5 during the
  stats window.
- `system.ruby.network.int_links12.network_link.flits_per_vnet::vnet-3`
  This is the number of data-vnet flits that crossed one specific internal
  link during the window.

### Units And Sampling Semantics

Most interpretation mistakes happen because different families use different
sampling rules.
Keep these categories separate:

- `*.TBEs.avg_size` and `*.<buffer>.m_buf_msgs` are time-average occupancies,
  meaning area under the occupancy curve divided by the window length
- `*.TBEs.avg_util` and `*.<buffer>.m_occupancy` are normalized versions of
  those occupancies, divided by configured capacity
- `system.ruby.m_outstandReqHistSeqr` is issue-sampled concurrency, not a
  time-average occupancy
- `packet_network_latency` and `flit_network_latency` are accumulated latency
  sums, not averages, so you must divide by completed packets or flits to get
  a mean
- `crossbar_activity`, `buffer_reads`, `buffer_writes`, `m_msg_count`, and
  per-link `flits_per_vnet` are raw window totals
- histogram `::mean` is the arithmetic mean of recorded samples, while
  histogram `::total` is sample count, not the sum of bucket values
When comparing runs, normalize raw counts to a common denominator such as
window cycles, measured operations, or received flits.

## Reference By Stat Family

### Ruby Global Profiler Stats

These are the top-level `system.ruby.*` profiler stats defined in
`src/mem/ruby/profiler/Profiler.cc`.
They aggregate data from all relevant controllers and sequencers.
They are sampled per completed request or per profiled message, not per cycle.
The most important piece of background is that the sequencer does not always
emit one latency sample per CPU memory instruction.
If several CPU accesses merge onto one outstanding Ruby miss, Ruby usually
records the request-level latency for the first request that actually enters
Ruby.
That is why this section repeatedly says `non-aliased Ruby request`.
It means the request that actually allocated Ruby work, not every later CPU
access that attached to it.

| Stat family | Meaning | Source |
|---|---|---|
| `system.ruby.m_outstandReqHistSeqr` | Distribution of outstanding non-aliased Ruby requests at the moment a new non-aliased request is inserted by a CPU sequencer | `Sequencer::insertRequest()` samples `m_outstanding_count` only for the non-aliased request actually issued to Ruby |
| `system.ruby.m_latencyHistSeqr` | Distribution of total latency for non-aliased Ruby requests issued by CPU sequencers | `Sequencer::recordMissLatency()`, called once for the first Ruby request while aliased followers are completed without another Ruby-latency sample |
| `system.ruby.m_hitLatencyHistSeqr` | Distribution of non-aliased Ruby requests satisfied without an external machine access | `recordMissLatency()`, `isExternalHit == false` branch |
| `system.ruby.m_missLatencyHistSeqr` | Distribution of non-aliased Ruby requests that required an external machine access | `recordMissLatency()`, `isExternalHit == true` branch |
| `system.ruby.delayHistogram` | Distribution of controller message delays accumulated through `AbstractController::profileMsgDelay()` and collated into the global profiler | `AbstractController.cc`, `Profiler::collateStats()` |
| `system.ruby.delayVCHist.vnet_<n>` | Same message-delay histogram, split per virtual network | `AbstractController.cc`, `Profiler::collateStats()` |

Use these when you want the global shape of the memory system rather than a
single object's local bottleneck.
Do not use `m_outstandReqHistSeqr::mean` as an exact time-average occupancy.
It is issue-sampled concurrency, not a per-cycle average.
If you need a time-average occupancy, prefer `*.TBEs.avg_size`.
`delayVCHist.vnet_<n>` is source-defined but may be absent in a given dump
because of `nozero` suppression or because that workload never sampled
controller message delays on that vnet.

#### Request-Type Latency Histograms

The profiler also emits histograms by `RubyRequestType`.
The names look like this:

- `system.ruby.RequestType.<RubyRequestType>.latency_hist_seqr`
- `system.ruby.RequestType.<RubyRequestType>.hit_latency_hist_seqr`
- `system.ruby.RequestType.<RubyRequestType>.miss_latency_hist_seqr`

Observed request types in the validated runs included `LD`, `ST`, and
`IFETCH`.
These histograms answer:

- Are loads slower than instruction fetches?
- Are stores mostly local hits or coherence misses?
- Does one request class dominate the long tail?

#### Machine-Type Latency Histograms

The profiler source also defines machine-type and request-machine-type
histograms.
The families are:

- `system.ruby.MachineType.<MachineType>.hit_mach_latency_hist_seqr`
- `system.ruby.MachineType.<MachineType>.miss_mach_latency_hist_seqr`
- `system.ruby.MachineType.<MachineType>.miss_latency_hist_seqr.issue_to_initial_request`
- `system.ruby.MachineType.<MachineType>.miss_latency_hist_seqr.initial_to_forward`
- `system.ruby.MachineType.<MachineType>.miss_latency_hist_seqr.forward_to_first_response`
- `system.ruby.MachineType.<MachineType>.miss_latency_hist_seqr.first_response_to_completion`
- `system.ruby.MachineType.<MachineType>.incomplete_times_seqr`
- `system.ruby.RequestTypeMachineType.<RubyRequestType>.<MachineType>.hit_type_mach_latency_hist_seqr`
- `system.ruby.RequestTypeMachineType.<RubyRequestType>.<MachineType>.miss_type_mach_latency_hist_seqr`
- `system.ruby.RequestTypeMachineType.<RubyRequestType>.<MachineType>.miss_type_mach_latency_hist_coalsr`

These families are source-defined rather than universally emitted.
In the fresh `hop_latency`, `hotspot`, `hotspot-heavy`, and
`link-pressure` reruns used for this reference, none of the
`MachineType.*` or `RequestTypeMachineType.*` families were printed.
Treat them as optional stats whose presence depends on the profiler path
being populated in that configuration and measurement window.
These are the most precise global latency-decomposition stats Ruby offers.
Like the base sequencer histograms, they are keyed to non-aliased Ruby
requests rather than every CPU-side access that may have coalesced onto that
request.
The `*_coalsr` variants are mainly relevant for GPU-coalescer configurations
and are usually absent in CPU-only CHI runs like the ones validated here.
`incomplete_times_seqr` is especially important.
If it is non-zero, the phase histograms for that machine type did not have a
complete timestamp chain for every sample, so the phase totals will not
reconstruct the full miss latency exactly.

### Controller Event And Transition Stats

These stats tell you how often protocol events and transitions happened, and
how long selected controller-local transition paths took.
For a reader who is less familiar with SLICC, think of a controller as a local
protocol state machine.
An event stat tells you how many times that state machine processed a named
input.
A state-qualified event stat tells you how many times it processed that input
while it was in one specific state.
A transition-latency histogram tells you how long one recorded local
controller action took from the controller's point of view.

#### Aggregate Controller Event Vectors

These are generated in the built CHI controller code under
`build/RISCV/mem/ruby/protocol/CHI/*_Controller.cc`.
They are global aggregates, placed under `system.ruby.*`, with one vector
element per controller instance of that machine type.
Only controller instance `0` of a given machine type creates these aggregate
vectors.
They live in the global profiler namespace and collate all sibling
controllers of that machine type.

##### Event-Count Vectors

Families:

- `system.ruby.Cache_Controller.<Event>`
- `system.ruby.Memory_Controller.<Event>`
- `system.ruby.MiscNode_Controller.<Event>`

What they mean:

- each vector index is one controller instance of that machine type
- the row shows how the event count is distributed across those instances
- `::total` is the total number of times that event fired across all
  instances in the window
In this CHI configuration, `Cache_Controller` is especially broad.
It aggregates all CHI cache-controller instances, including private
cache-side controllers and home-node cache controllers.
Example families observed in the CHI cache-controller aggregate stats
include:

- `AllocRequest`
- `AllocSeqRequest`
- `Load`
- `ReadShared`
- `CompAck`
- `CompData_SC`
- `TagArrayRead`
- `FillPipe`
- `SendReadShared`
- `Final`

Important interpretation rule:
These are protocol-event counts, not request counts.
One demand miss may trigger many internal events.
That is why values such as
`system.ruby.Cache_Controller.CheckCacheFill::total` can be far larger than
`cache.m_demand_accesses`.

##### State-Qualified Transition-Count Vectors

Families:

- `system.ruby.Cache_Controller.<State>.<Event>`
- `system.ruby.Memory_Controller.<State>.<Event>`
- `system.ruby.MiscNode_Controller.<State>.<Event>`

What they mean:

- the controller was in `<State>`
- event `<Event>` was processed
- the counter increments on that `(state, event)` transition
- the vector is again split by controller instance

These are the best stats for answering questions such as:

- Are most `ReadShared` requests arriving while the controller is already in
  `BUSY_INTR` or `BUSY_BLKD`?
- Are evictions mostly coming from `SC` or `UC_RSC` states?
- Is a retry-heavy workload stuck in a small subset of transient states?

#### Per-Controller Transition Latency Histograms

These are local stats owned by each SLICC-generated controller instance.
They are defined in the generated controller code, not in the global
profiler.
You will see them on concrete controller objects such as:

- `system.cpu0.l1d.*`
- `system.cpu0.l1i.*`
- `system.cpu0.l2.*`
- `system.ruby.hnf15.cntrl.*`

The families are:

- `*.outTransLatHist.<Event>`
- `*.outTransLatHist.<Event>.retries`
- `*.inTransLatHist.<Event>`
- `*.inTransLatHist.<Event>.retries`
- `*.inTransLatHist.<Event>.<InitialState>.<FinalState>.total`

What they mean:

- `outTransLatHist.<Event>` records latency for selected outbound transition
  events generated by that controller
- `inTransLatHist.<Event>` records latency for selected incoming transition
  events handled by that controller
- the `.retries` scalar is the retry-pressure companion for that event family,
  so it counts how often recorded instances of that event had to retry rather
  than making immediate forward progress
- `.<InitialState>.<FinalState>.total` records how many times that incoming
  event ended with that exact state change
These stats are invaluable because they stay local.
They tell you whether the pain is in CPU0's L1D, CPU0's L2, or the hot HNF
itself.
Example from the validated `hotspot-heavy` measured block:

- `system.cpu0.l1d.outTransLatHist.SendReadShared::mean = 481.22`
- `system.cpu0.l1d.inTransLatHist.Load::mean = 353.17`
- `system.cpu0.l1d.inTransLatHist.Load.I.SC.total = 8192`

This means the benchmark's main path is a large set of `Load` transitions
entering from state `I` and ending in `SC`, which matches the read-shared
hot-line workload.

#### Controller Busy And Delay Stats

These are defined in
`src/mem/ruby/slicc_interface/AbstractController.cc`.
Families:

- `*.fullyBusyCycles`
- `*.delayHistogram`

What they mean:

- `fullyBusyCycles` counts cycles where the controller hit its
  `transitions_per_cycle` limit
- `delayHistogram` samples message delay values passed into
  `AbstractController::profileMsgDelay()`
Use `fullyBusyCycles` to detect controller scheduling saturation.
If it stays near zero while network buffers explode, the controller is not
your bottleneck.
These stats often disappear from the dump because they are flagged `nozero`.

### Controller Occupancy And Queueing Stats

These families are the most direct view of local pressure inside Ruby.
They also measure more persistent state than the event counters above.
A TBE is a transient record for an in-flight miss or coherence transaction.
A `MessageBuffer` is a queue of Ruby protocol messages between controllers,
sequencers, and the network.
That is why many of the stats in this section are time-average occupancies
rather than simple event counts.

#### TBE Storage Stats

These are defined in `src/mem/ruby/structures/TBEStorage.cc`.
Families:

- `*.TBEs.avg_size`
- `*.TBEs.avg_util`
- `*.TBEs.avg_reserved`

And protocol-specific sibling pools often exist too:

- `*.replTBEs.*`
- `*.snpTBEs.*`
- `*.dvmTBEs.*`
- `*.dvmSnpTBEs.*`

What they mean:

- `avg_size` is the time-average number of allocated slots
- `avg_util` is `avg_size / configured_capacity`
- `avg_reserved` is the time-average number of reserved slots

Operationally, a high `avg_size` means the controller spends much of the
window with many transactions still in flight.
A high `avg_reserved` means the controller is holding capacity aside for work
that is not yet fully active.
These are time averages, so they are valid occupancy inputs for Little's Law
reasoning.
Example from the validated `hotspot-heavy` measured block:

- `system.ruby.hnf15.cntrl.TBEs.avg_size = 36.05`
- `system.ruby.hnf15.cntrl.TBEs.avg_util = 0.563`

That tells you HNF15 is busy, but not full, under the single-hot-HNF run.

#### CacheMemory Stats

These are defined in `src/mem/ruby/structures/CacheMemory.cc`.
Families:

- `*.cache.numDataArrayReads`
- `*.cache.numDataArrayWrites`
- `*.cache.numTagArrayReads`
- `*.cache.numTagArrayWrites`
- `*.cache.numTagArrayStalls`
- `*.cache.numDataArrayStalls`
- `*.cache.numAtomicALUOperations`
- `*.cache.numAtomicALUArrayStalls`
- `*.cache.m_demand_hits`
- `*.cache.m_demand_misses`
- `*.cache.m_demand_accesses`
- `*.cache.m_prefetch_hits`
- `*.cache.m_prefetch_misses`
- `*.cache.m_prefetch_accesses`
- `*.cache.m_accessModeType::*`

Exact formulas verified in source:

- `m_demand_accesses = m_demand_hits + m_demand_misses`
- `m_prefetch_accesses = m_prefetch_hits + m_prefetch_misses`

How to use them:

- use `m_demand_accesses` to identify which cache or HNF actually owns the
  traffic
- use hit vs miss split to decide whether the bottleneck is in-cache or
  downstream
- use array read and write counts when you need microarchitectural activity
  rather than just request counts
For the two validated workloads, the most useful member of this family was
`*.cache.m_demand_accesses`.
Examples:

- `system.ruby.hnf15.cntrl.cache.m_demand_accesses = 8199` in the measured
  `hotspot-heavy` block
- `system.ruby.hnf<i>.cntrl.cache.m_demand_accesses` was used to verify
  uniform HNF spreading in `link-pressure`

#### MessageBuffer Stats

These are defined in `src/mem/ruby/network/MessageBuffer.cc`.
Families:

- `*.<buffer>.m_not_avail_count`
- `*.<buffer>.m_msg_count`
- `*.<buffer>.m_buf_msgs`
- `*.<buffer>.m_stall_time`
- `*.<buffer>.m_stall_count`
- `*.<buffer>.m_avg_stall_time`
- `*.<buffer>.m_occupancy`

Exact meanings from source:

- `m_not_avail_count`: number of times the buffer did not have enough free
  slots for an enqueue request
- `m_msg_count`: number of messages that passed through the buffer
- `m_buf_msgs`: average number of messages resident in the buffer over time
- `m_stall_time`: total ticks messages spent stalled in that buffer
- `m_stall_count`: number of stall events
- `m_avg_stall_time = m_stall_time / m_msg_count`, so it is average stall
  ticks per message that passed through the buffer, not average stall ticks
  per stalled message
- `m_occupancy = m_buf_msgs / max_size` for bounded buffers, otherwise `0`

This is the most useful family for pinpointing where queueing physically
lives inside Ruby.
These counts are in messages, not flits.
They tell you about queueing in the Ruby protocol layer before or after
network serialization.
Use `m_buf_msgs` when you want a stable queue-depth signal.
Use `m_msg_count` when you want traffic volume.
Do not confuse them.
`m_msg_count` is throughput.
`m_buf_msgs` is occupancy.
Common buffer names observed in these runs were:

| Controller class | Concrete buffer names observed |
|---|---|
| L1D | `mandatoryQueue`, `replTriggerQueue`, `reqOut`, `reqRdy`, `rspIn`, `rspOut`, `datIn`, `datOut`, `triggerQueue` |
| L2 | `reqIn`, `reqOut`, `reqRdy`, `rspIn`, `rspOut`, `snpOut`, `datIn`, `datOut`, `replTriggerQueue`, `triggerQueue` |
| HNF `cntrl` | `reqIn`, `reqOut`, `reqRdy`, `rspIn`, `rspOut`, `datIn`, `datOut`, `triggerQueue` |

Because these stats are per concrete message buffer, they are usually the
fastest way to answer questions such as:

- Is queueing at the source controller or inside the network?
- Which output class is congested, `reqOut`, `rspOut`, or `datOut`?
- Is the pressure local to one HNF or spread across many objects?

### Garnet Network-Wide Stats

These are defined in `src/mem/ruby/network/garnet/GarnetNetwork.cc` and
updated by `NetworkInterface.cc`.
Garnet adds another layer of object names that are worth separating up front.
A Ruby protocol message arrives at the network interface and becomes a packet.
That packet is then broken into one or more flits for movement through
routers, links, and virtual channels.
Control traffic is often one flit per packet.
Data traffic often spans several flits per packet.
So packet counters tell you how many messages completed at the NI, while flit
counters tell you how much work the network fabric actually carried.

#### Packet And Flit Counters

Families:

- `system.ruby.network.packets_injected`
- `system.ruby.network.packets_received`
- `system.ruby.network.flits_injected`
- `system.ruby.network.flits_received`

These are per-vnet vectors.
`::total` is the sum across vnets.
If one packet uses five flits, it contributes `1` to `packets_received` and
`5` to `flits_received`.
Important caveat:
Injected and received totals do not have to match exactly inside a finite
stats window.
If the window ends with traffic still in flight, the counts can differ
slightly.
That behaviour was visible in the validated runs.

#### Latency Accumulators

Families:

- `system.ruby.network.packet_network_latency`
- `system.ruby.network.packet_queueing_latency`
- `system.ruby.network.flit_network_latency`
- `system.ruby.network.flit_queueing_latency`

These are per-vnet accumulated tick counts, not averages.
The averages are computed from them.
Conceptually, every received flit contributes two delay pieces.
One piece is time spent moving through the network fabric itself.
The other piece is time spent waiting in queues at the source or destination
side of the NI path.
Exact flit semantics from `NetworkInterface::incrementStats()`:

- `network_delay = dequeue_time - enqueue_time - 1 cycle`
- `src_queueing_delay = t_flit->get_src_delay()`
- `dest_queueing_delay = current_tick - dequeue_time`
- `queueing_delay = src_queueing_delay + dest_queueing_delay`

So `flit_network_latency` is time spent traversing routers and links after
injection.
And `flit_queueing_latency` is source-NI queueing plus destination-side
queueing.
Packet latencies are updated only for `TAIL_` or `HEAD_TAIL_` flits.
That means packet latency is effectively measured at packet completion.

#### Average Latencies

Families:

- `system.ruby.network.average_packet_vnet_latency`
- `system.ruby.network.average_packet_vqueue_latency`
- `system.ruby.network.average_packet_network_latency`
- `system.ruby.network.average_packet_queueing_latency`
- `system.ruby.network.average_packet_latency`
- `system.ruby.network.average_flit_vnet_latency`
- `system.ruby.network.average_flit_vqueue_latency`
- `system.ruby.network.average_flit_network_latency`
- `system.ruby.network.average_flit_queueing_latency`
- `system.ruby.network.average_flit_latency`

Exact formulas from source:

- `average_flit_vnet_latency = flit_network_latency / flits_received`
- `average_flit_vqueue_latency = flit_queueing_latency / flits_received`
- `average_flit_network_latency = sum(flit_network_latency) /
  sum(flits_received)`
- `average_flit_queueing_latency = sum(flit_queueing_latency) /
  sum(flits_received)`
- `average_flit_latency = average_flit_network_latency +
  average_flit_queueing_latency`
The packet versions follow the same pattern with packet counters.
In plain language, a per-vnet average such as
`average_flit_vqueue_latency::vnet-3` means:
the average queueing delay seen by one completed flit on vnet `3` during this
measurement window.
Use the per-vnet vectors first.
The all-vnet scalar average hides which traffic class is actually suffering.
Example from the `link-pressure` 16-thread per-vnet block:

- `average_flit_network_latency = 15121.50`
- `average_flit_queueing_latency = 27157.02`
- `average_flit_vqueue_latency = |1979.80 | 567.49 | 687.84 | 43664.47|`

That immediately tells you the data vnet is the queueing victim.

#### Hop Count

Families:

- `system.ruby.network.average_hops`

This is `total_hops / flits_received_total`.
It is an average flit hop count, not a packet hop count.
Use it to distinguish topological distance effects from pure queueing effects.

#### Link Utilization And VC Load

Families:

- `system.ruby.network.ext_in_link_utilization`
- `system.ruby.network.ext_out_link_utilization`
- `system.ruby.network.int_link_utilization`
- `system.ruby.network.avg_link_utilization`
- `system.ruby.network.avg_vc_load`

This family is easy to misuse.
Here is the exact source behaviour.
`GarnetNetwork::collateStats()` loops over every flit-carrying network link.
For each link, it reads `activity = link->getLinkUtilization()`, where
activity is the count of flits that traversed that link during the window.
It then:

- adds the raw count into `ext_in_link_utilization`,
  `ext_out_link_utilization`, or `int_link_utilization`
- adds `activity / time_delta` into `avg_link_utilization`
- adds each VC's `vc_load / time_delta` into `avg_vc_load`

This implies three critical interpretation rules.

1. `ext_in_link_utilization`, `ext_out_link_utilization`, and
   `int_link_utilization` are raw aggregate flit counts, not normalized
   utilizations.

2. `avg_link_utilization` is an aggregate flits-per-cycle over all flit links
   in the network.
It is not a 0-to-1 occupancy fraction of one link.
That is why it can be much larger than `1`.
Think of it as network throughput summed across links, not as one-link
fullness.

3. `avg_vc_load::total` must equal `avg_link_utilization` for the same block,
   because it is the same total activity partitioned by VC.
This was directly verified in the validated runs.
For the `link-pressure` 16-thread per-vnet block:

- `avg_link_utilization = 81.566798`
- `avg_vc_load::total = 81.566798`
- `ext_in + ext_out + int = 30974510 + 30974532 + 53355741`
- dividing that sum by the measured window cycles gives the same `81.57`
  flits per cycle
That one example is enough to prove these are aggregate rates, not per-link
percentages.

#### Traffic Distribution Matrices

Families:

- `system.ruby.network.data_traffic_distribution.n<src>.n<dst>`
- `system.ruby.network.ctrl_traffic_distribution.n<src>.n<dst>`

These are packet counts indexed by source router and destination router.
They are updated in `GarnetNetwork::update_traffic_distribution()`.
The split is by vnet type:

- data vnet packets increment `data_traffic_distribution`
- all other packets increment `ctrl_traffic_distribution`

Use these when you want to answer structural questions such as:

- Is traffic uniformly spread or concentrated on a few source-destination
  pairs?
- Is the data plane balanced while the control plane is skewed?
- Does a topology or routing change alter path destinations the way you
  expected?
In the fresh `link-pressure` reruns, the summed matrix counts tracked the
received packet split closely but were not bit-for-bit identical.
Use these as structural distribution counters, not as a stricter replacement
for `packets_received`.

### Garnet Per-Router And Per-Link Stats

#### Per-Router Stats

These are defined in `src/mem/ruby/network/garnet/Router.cc`.
Families:

- `system.ruby.network.routersXX.buffer_reads`
- `system.ruby.network.routersXX.buffer_writes`
- `system.ruby.network.routersXX.crossbar_activity`
- `system.ruby.network.routersXX.sw_input_arbiter_activity`
- `system.ruby.network.routersXX.sw_output_arbiter_activity`

What they mean:

- `buffer_reads` and `buffer_writes` count Garnet input-buffer activity
  collected from the input units
- `crossbar_activity` counts flits that traversed that router's crossbar
- `sw_input_arbiter_activity` and `sw_output_arbiter_activity` count
  allocator activity
These are raw counts over the stats window.
To compare routers fairly, divide by the window length in cycles.
Do not assume the router count equals the visible mesh-tile count.
In this CHI custom-mesh configuration, the stats show router IDs beyond
`0..15` because the topology includes additional node-side routers as well as
the 4x4 mesh routers.
For the `link-pressure` 16-thread per-vnet block, the busiest mesh routers
were obvious after that normalization.
That is the correct way to build a heat map.

#### Per-Link Stats

These are defined in `src/mem/ruby/network/garnet/NetworkLink.cc`.
Families:

- `system.ruby.network.int_linksXX.network_link.flits_per_vnet::*`
- `system.ruby.network.ext_linksXX.network_links0.flits_per_vnet::*`
- `system.ruby.network.ext_linksXX.network_links1.flits_per_vnet::*`
- `system.ruby.network.*.credit_links*.flits_per_vnet::*`

What they mean:

- every flit-carrying network link tracks flits transmitted per vnet
- every credit link also inherits the same stat

This distinction is essential.
`network_links*` carry traffic.
`credit_links*` carry backpressure credits.
One subtle but important implementation detail is that `Credit` flits are
constructed with `vnet = 0` in
`src/mem/ruby/network/garnet/Credit.cc`.
So credit-link `flits_per_vnet` rows are not a faithful breakdown of request,
snoop, response, and data traffic classes.
They are mainly useful as a credit-return activity signal.
Do not sum them together when estimating useful traffic volume.
Use `network_links*` for throughput and hotspot diagnosis.
Use `credit_links*` when you want to verify flow-control behaviour or credit
return intensity.

## Analysis Workflow

This workflow is the shortest reliable path from raw `stats.txt` output to a
defensible diagnosis.
Use it in order.

### Step 1. Lock Down The Measurement Window

Pick the intended dumped block first.
If a benchmark emits several measured blocks, map each block to the phase,
thread count, traffic point, or parameter setting that produced it.
Then compare like with like.
Do not compare a warmup block against a steady-state block, or one sweep point
against another, unless that contrast is the thing you are studying.
Normalize all raw counters to the same denominator before comparing runs.
Good normalizations are:

- per CPU cycle: `count / measured_cycles`
- per operation: `count / measured_ops`
- per received flit: `latency_sum / flits_received_total`, which Garnet
  already prints as average latencies

### Step 2. Decide Whether The Pressure Is Localized Or Distributed

Start with service-point locality.
Inspect:

- `system.ruby.hnf*.cntrl.cache.m_demand_accesses`
- `*.TBEs.avg_size`
- `*.cache.m_demand_hits` and `*.cache.m_demand_misses`

Interpretation:

- one HNF dominating means a service-point hotspot
- uniform HNF demand means the fabric is a more likely bottleneck

This is the fastest first split between localized service pressure and
fabric-wide pressure.

### Step 3. Split Local Queueing From Network Queueing

Use both Ruby-local and Garnet-global queues.
Inspect:

- `*.<buffer>.m_buf_msgs`
- `*.<buffer>.m_stall_time`
- `system.ruby.network.average_flit_network_latency`
- `system.ruby.network.average_flit_queueing_latency`
- `system.ruby.network.average_flit_vnet_latency`
- `system.ruby.network.average_flit_vqueue_latency`
- `system.ruby.network.average_packet_vnet_latency`
- `system.ruby.network.average_packet_vqueue_latency`

Interpretation:

- large `m_buf_msgs` on a controller output buffer means queueing before
  injection
- large `average_flit_vqueue_latency` with low controller buffer depths means
  NI and network-side queueing dominate
- large `average_flit_vnet_latency` with modest queueing means path length or
  router/link service time dominates
- large packet-vs-flit gaps usually indicate serialization effects and
  multi-flit data packets rather than only more hops

### Step 4. Identify The Pressure Class By Vnet

Always inspect vnet splits before drawing conclusions.
Use:

- `system.ruby.network.flits_received`
- `system.ruby.network.packets_received`
- `system.ruby.network.average_flit_vnet_latency`
- `system.ruby.network.average_flit_vqueue_latency`
- `system.ruby.network.average_packet_vnet_latency`
- `system.ruby.network.average_packet_vqueue_latency`
- per-link `flits_per_vnet`

In this CHI configuration:

- vnet `0` is request
- vnet `1` is snoop
- vnet `2` is response
- vnet `3` is data

If one vnet dominates both volume and queueing, optimize that class first.

### Step 5. Localize Hotspots To Routers And Links

Once you know which vnet is suffering, map it onto the fabric.
Use:

- `system.ruby.network.routersXX.crossbar_activity`
- `system.ruby.network.routersXX.buffer_reads`
- `system.ruby.network.int_linksXX.network_link.flits_per_vnet::*`
- `system.ruby.network.ext_linksXX.network_links*.flits_per_vnet::*`
- `system.ruby.network.data_traffic_distribution.*`
- `system.ruby.network.ctrl_traffic_distribution.*`

Recommended derived metrics:

```text
router_crossbar_flits_per_cycle = router.crossbar_activity / window_cycles
link_flits_per_cycle = link.flits_per_vnet::total / window_cycles
vnet_share = flits_received::vnet-k / flits_received::total
```

This tells you whether the problem is:

- one hot source link
- one hot interior router
- one edge row or column
- a general all-to-all saturation pattern

### Step 6. Distinguish Fabric Saturation From Service-Point Saturation

Use this decision rule.
If `avg_link_utilization` is high, hot routers are widespread, and HNF demand
is uniform, you are looking at a fabric bottleneck.
If one HNF's `TBEs.avg_size` and local output buffers dominate while global
network utilization remains modest, you are looking at a service-point
bottleneck.
The worked examples below show both cases clearly.

### Step 7. Use Transition Histograms To Tie Performance Back To Protocol Behaviour

Once you know where the pressure lives, use transition stats to learn why.
Use:

- `system.ruby.Cache_Controller.<Event>::total`
- `system.ruby.Cache_Controller.<State>.<Event>::total`
- local `*.inTransLatHist.<Event>`
- local `*.outTransLatHist.<Event>`

Questions these answer:

- Is the workload dominated by `ReadShared` or by writebacks and evictions?
- Are retries happening on incoming demand events or on outgoing network
  sends?
- Which stable or transient state is responsible for most of the event
  traffic?
This step is where you connect performance back to the protocol state machine
rather than only to the network.

### Step 8. Cross-Check With Sequencer Histograms

Use:

- `system.ruby.m_latencyHistSeqr`
- `system.ruby.RequestType.<RubyRequestType>.latency_hist_seqr`
- `system.ruby.MachineType.<MachineType>.miss_mach_latency_hist_seqr`
- `system.ruby.MachineType.<MachineType>.miss_latency_hist_seqr.*`

These global histograms answer whether your local diagnosis is visible at the
requester too.
If controller-local queues explode but sequencer latencies stay flat, you
probably looked at a non-critical path.
If both rise together, you found a real bottleneck.

### Step 9. Avoid Three Common Mistakes

#### Mistake 1. Treating `avg_link_utilization` as a per-link percentage

This is wrong.
It is aggregate flits per cycle across all flit links.
Values far above `1` are expected.

#### Mistake 2. Treating `m_outstandReqHistSeqr::mean` as a time-average occupancy

This is wrong.
It is sampled when a new non-aliased Ruby request is inserted.
Coalesced followers do not add another Ruby-latency or outstanding-request
sample.
Use TBE `avg_size` when you need a true time-average occupancy.

#### Mistake 3. Treating `system.ruby.Cache_Controller.<Event>::total` as demand-request count

This is wrong.
Those are internal protocol event counts across all cache-controller
instances.
One request can trigger many such events.

## Worked Examples

### `hotspot-heavy`

Measured block facts:

- `system.ruby.hnf15.cntrl.cache.m_demand_accesses = 8199`
- `system.ruby.hnf15.cntrl.TBEs.avg_size = 36.05`
- `system.ruby.network.average_flit_network_latency = 17115.81`
- `system.ruby.network.average_flit_queueing_latency = 46011.32`
- `system.ruby.network.avg_link_utilization = 8.615`

Interpretation:

- the network is active, but not globally saturated
- queueing is much larger than transit time
- the single hot home node dominates service-side pressure

So this workload is primarily a service-point hotspot, not a whole-mesh
link-saturation test.

### `link-pressure`: 16-Thread Shared vs Per-Vnet

Shared-links measured block:

- throughput `0.92806` line ops/cycle from `console.log`
- `average_flit_network_latency = 16624.56`
- `average_flit_queueing_latency = 28124.29`
- `avg_link_utilization = 68.57`

Per-vnet-links measured block:

- throughput `1.11267` line ops/cycle from `console.log`
- `average_flit_network_latency = 15121.50`
- `average_flit_queueing_latency = 27157.02`
- `avg_link_utilization = 81.57`

Interpretation:

- the per-vnet run carries more traffic while also improving throughput
- aggregate network activity is high in both runs
- the system is fabric-limited enough that separating physical links by vnet
  helps materially
This is the signature of network pressure, not a single overloaded HNF.

## Validation And Sources

This reference was revalidated against fresh reruns of the workload set used
to spot-check names, block structure, and the main interpretation rules:

- `ruby-book/final/hop_latency`
- `ruby-book/final/hotspot`
- `ruby-book/final/hotspot-heavy`
- `ruby-book/final/link-pressure` with both shared links and
  `--per-vnet-links`
The runs used for validation were:

| Workload | Command | Outdir | Blocks | Validation purpose |
|---|---|---|---|---|
| `hop_latency` | `make -C ruby-book/final/hop_latency report` | `m5out/rbook-hop-latency-20260421-074743` | `3` | Near-vs-far path isolation, block handling, and per-link stat naming |
| `hotspot` | `make -C ruby-book/final/hotspot report` | `m5out/rbook-hotspot-20260421-075058` | `6` | Multi-block hotspot sweep, destination skew, and router/link activity |
| `hotspot-heavy` | `make -C ruby-book/final/hotspot-heavy report` | `m5out/rbook-hotspot-heavy-20260421-075346` | `2` | Single-hot-HNF case, queueing concentrated near one service point |
| `link-pressure` shared | `SIM_TIMEOUT=15m make -C ruby-book/final/link-pressure compare` | `m5out/rbook-link-pressure-shared-20260421-075412` | `6` | Mesh-link contention with shared physical links |
| `link-pressure` per-vnet | same compare target | `m5out/rbook-link-pressure-pervnet-20260421-080130` | `6` | Same workload with separated physical links per vnet |

Unless stated otherwise, numeric examples in this reference come from those
runs' `stats.txt` files, with most of the concrete performance examples drawn
from `hotspot-heavy` and `link-pressure`.
These reruns also reconfirmed two assumptions used in the main text:

- in the 16-thread `link-pressure` shared and per-vnet measured blocks,
  `avg_vc_load::total` exactly matched `avg_link_utilization`
- credit-link `flits_per_vnet` rows printed only `vnet-0` plus `::total`,
  while traffic-carrying network links printed `vnet-0` through `vnet-3`
When benchmark throughput is relevant, this reference cites the matching
`console.log` lines explicitly.
The implementation sources checked while writing this reference were:

- `src/mem/ruby/profiler/Profiler.cc`
- `src/mem/ruby/slicc_interface/AbstractController.cc`
- `src/mem/ruby/network/MessageBuffer.cc`
- `src/mem/ruby/structures/TBEStorage.cc`
- `src/mem/ruby/structures/CacheMemory.cc`
- `src/mem/ruby/network/garnet/GarnetNetwork.cc`
- `src/mem/ruby/network/garnet/NetworkInterface.cc`
- `src/mem/ruby/network/garnet/Router.cc`
- `src/mem/ruby/network/garnet/NetworkLink.cc`
- `src/mem/ruby/system/Sequencer.cc`
- `src/mem/ruby/protocol/chi/CHI-cache.sm`
- `src/mem/ruby/protocol/chi/CHI-mem.sm`
- generated CHI controller code under
  `build/RISCV/mem/ruby/protocol/CHI/*_Controller.cc`
