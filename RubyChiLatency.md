# Profiling CHI Transaction Latency in Ruby

This note summarizes what the current gem5 Ruby/CHI codebase can already
answer about transaction latency, where the answers appear, and which patches
are worth adding if you need exact per-transaction attribution, exact min/max,
or controller-local statistics such as "L2 Cache controller in Tile 0".

## Short Answer

Current gem5 already has three useful latency views:

1. **Sequencer request latency**: CPU-side Ruby request lifetime, from
   `Sequencer::insertRequest()` to `readCallback`/`writeCallback` completion.
   This answers "how long did a request from the sequencer mandatory queue take
   until completion?" at aggregated statistical level. These stats are always
   on (no flag needed) and are dumped to `m5out/stats.txt` directly under
   `system.ruby.` — **not** `system.ruby.profiler.`, which does not exist. See
   "Existing Tool 1" for the exact names and why a run can show nothing.

2. **CHI controller transaction latency**: protocol-level CHI `in_trans` and
   `out_trans` histograms generated from SLICC annotations and sampled by
   `AbstractController::incomingTransactionStart/End()` and
   `outgoingTransactionStart/End()`. This is the closest existing machinery for
   questions like "L2 sent `ReadShared`; how long until that outgoing
   transaction completed?" and "HN-F received `ReadShared`; how long did the
   home-side transaction take?"

3. **Network packet/flit latency**: Garnet exposes average packet/flit network
   and queueing latency. SimpleNetwork exposes message counts/bytes, buffer
   occupancy/stall statistics, and an optional route profiler, but not a normal
   packet latency histogram.

For exact individual transactions, exact min/max, and easy per-node queries,
the current code is not enough. The best patch is a small Ruby/CHI transaction
trace/stat layer keyed by controller, event, address or CHI transaction ID, and
start/end tick. For statistical min/max, add explicit `Scalar`/`Vector` or
`Distribution` stats next to the existing histograms, because the current
histograms mainly print bucket ranges and means, not exact min/max values.

## Existing Tool 1: Sequencer Request Latency

Relevant code:

- `src/mem/ruby/system/Sequencer.hh`
- `src/mem/ruby/system/Sequencer.cc`
- `src/mem/ruby/profiler/Profiler.hh`
- `src/mem/ruby/profiler/Profiler.cc`
- `src/mem/ruby/system/RubySystem.cc` — creates the `Profiler`, registers the
  stats dump callback
- `src/base/stats/group.cc` — explains the stat-name prefix (see below)

### What it measures

The sequencer creates a `SequencerRequest` with `issue_time = curCycle()` in
`Sequencer::insertRequest()`. Completion is sampled in
`Sequencer::recordMissLatency()`, which the protocol callbacks
`readCallbackCBusy()`, `writeCallbackCBusy()`, and `atomicCallbackCBusy()`
invoke. `recordMissLatency()` computes `total_lat = curCycle() - issue_time`
(in Ruby controller cycles) and feeds it into per-sequencer histograms:

- Total CPU-facing Ruby request latency.
- Request-type histograms keyed by `RubyRequestType`.
- Hit/miss split (`isExternalHit`) as seen by the controller attached to the
  sequencer.
- Responding `MachineType` split, when the protocol passes a valid machine
  type (not `MachineType_NUM`).
- Miss-latency breakdown (issue -> initial -> forward -> first-response ->
  completion) when the protocol passes ordered `initialRequestTime`,
  `forwardRequestTime`, and `firstResponseTime`. If those timestamps are not
  monotonically ordered the sample is dropped and the per-machine
  `incomplete_times_seqr` counter is incremented instead.

### How to enable it

There is nothing to switch on. The sequencer latency profiler is always
compiled in and always recording. It is **not** behind a debug flag or a
parameter. Every Ruby run that issues CPU requests through a `Sequencer`
populates it.

Two things *are* gated, and they are easy to confuse with the latency
profiler:

- `RubySystem.hot_lines` and `RubySystem.all_instructions` only control the
  `AddressProfiler` (hot-address / per-instruction profiling). They do **not**
  gate the latency histograms. With both false (the default) you still get the
  full sequencer latency stats.
- `--debug-flags=ProtocolTrace` adds a human-readable completion line per
  request (printed from `recordMissLatency()`), but it does not change which
  stats are emitted.

### Where the stats are dumped (the part that trips people up)

The stats land in the normal gem5 statistics file: `m5out/stats.txt`, or
`<outdir>/stats.txt` if you passed `--outdir`. `RubySystem` registers a dump
callback (`statistics::registerDumpCallback([this]{ collateStats(); })`), so
the per-sequencer histograms are collated into the Ruby-system-level stats
every time stats are dumped.

**There is no `profiler` node in the stat path.** This is almost certainly why
`system.ruby.profiler*` matched nothing. The `Profiler::ProfilerStats` object
is constructed as `statistics::Group(parent)` with a parent but **no group
name**. In gem5's stats framework (`Group::Group` in
`src/base/stats/group.cc`), a group created with a parent and no name is
*merged* into the parent instead of being nested under it. The parent here is
the `RubySystem` itself. So every profiler stat is emitted directly under the
RubySystem SimObject, normally `system.ruby`:

```text
system.ruby.m_latencyHistSeqr            # all requests
system.ruby.m_hitLatencyHistSeqr         # requests that hit locally
system.ruby.m_missLatencyHistSeqr        # requests that needed external messages
system.ruby.m_outstandReqHistSeqr        # outstanding requests per cycle
system.ruby.delayHistogram               # message delay, all vnets
system.ruby.delayVCHist.vnet_<n>         # message delay per vnet
```

The per-request-type, per-machine-type, and per-(request,machine) breakdowns
*do* get named subgroups, again directly under the RubySystem:

```text
system.ruby.RequestType.<RubyRequestType>.latency_hist_seqr
system.ruby.RequestType.<RubyRequestType>.hit_latency_hist_seqr
system.ruby.RequestType.<RubyRequestType>.miss_latency_hist_seqr
system.ruby.MachineType.<MachineType>.hit_mach_latency_hist_seqr
system.ruby.MachineType.<MachineType>.miss_mach_latency_hist_seqr
system.ruby.MachineType.<MachineType>.miss_latency_hist_seqr.issue_to_initial_request
system.ruby.MachineType.<MachineType>.miss_latency_hist_seqr.initial_to_forward
system.ruby.MachineType.<MachineType>.miss_latency_hist_seqr.forward_to_first_response
system.ruby.MachineType.<MachineType>.miss_latency_hist_seqr.first_response_to_completion
system.ruby.MachineType.<MachineType>.incomplete_times_seqr
system.ruby.RequestTypeMachineType.<RubyRequestType>.<MachineType>.hit_type_mach_latency_hist_seqr
system.ruby.RequestTypeMachineType.<RubyRequestType>.<MachineType>.miss_type_mach_latency_hist_seqr
```

The `*Coalsr` variants of the above exist only in GPU builds (`BUILD_GPU`), for
the `GPUCoalescer`; on a CPU-only CHI build they are absent.

To locate them quickly:

```bash
grep -nE 'LatencyHistSeqr|latency_hist_seqr|outstandReqHistSeqr' m5out/stats.txt
```

### Why you might still see nothing

Even with the correct `system.ruby.` prefix, the histograms can be absent from
`stats.txt`:

1. **`nozero` flag.** Every profiler histogram is registered with
   `statistics::nozero`. A histogram with zero samples is omitted from
   `stats.txt` entirely — you get *no line at all*, not a line of zeros. No
   recorded requests means nothing to grep for.
2. **The sequencer must be a CPU sequencer.** `Profiler::collateStats()` only
   collates a controller's sequencer when
   `AbstractController::getCPUSequencer()` returns non-NULL, and the SLICC-
   generated code only returns it when `Sequencer::isCPUSequencer()` is true
   (the `is_cpu_sequencer` param, default `True`). DMA sequencers and any port
   with `is_cpu_sequencer=False` never contribute. `DMASequencer` is not just
   excluded from collation — it has no latency instrumentation at all (see
   Patch 4b).
3. **Requests must actually complete.** `recordMissLatency()` runs from the
   read/write/atomic callbacks. A request still outstanding when the simulation
   ends is never sampled.
4. **You may be looking at the wrong file.** Stats go to the run's `--outdir`
   (`m5out` by default), not the source tree, and only after a stats dump
   (end of run, or an explicit `m5 dumpstats`).

### The per-sequencer histograms are not individually named

The raw histograms live inside each `Sequencer` as plain
`statistics::Histogram` members constructed with **no name and no parent
group** (bare members and `new statistics::Histogram()` in `Sequencer.cc`,
only `.init(10)` is called). They are never registered as named stats, so they
never appear under the individual sequencer SimObject (you will not find
`system.cpu0.l1d.sequencer.m_latencyHist` or similar). They exist purely as
accumulators that `Profiler::collateStats()` sums into the merged
`system.ruby.*` stats.

Consequence: the shipped output is **system-wide only**. There is no
per-sequencer, per-core, or per-tile latency in `stats.txt` today. For
"Tile 0 only" numbers you must add per-sequencer stats — see Patch 4.

### Quick recipe to confirm the stats exist

```bash
# Any Ruby run, e.g. the bundled random tester:
build/NULL/gem5.opt configs/example/ruby_random_test.py \
    --num-cpus=2 --maxloads=2000

# Then look under system.ruby (NOT system.ruby.profiler):
grep -nE 'system\.ruby\.(m_.*HistSeqr|RequestType|MachineType)' m5out/stats.txt
```

If the grep is still empty, walk the four causes above in order.

Use this profiler when the question is endpoint-visible:

- "A CPU load/store entered Ruby; how long until Ruby completed it?"
- "What is the average Ruby request latency for loads?"
- "How does hit latency compare to miss latency?"

Limitations:

- The stats are collated across sequencers/controllers by `Profiler::collateStats()`.
  They do not give a clean per-RN-F or per-tile answer without adding per-object
  stats or scraping individual sequencer internals before collation.
- They do not directly represent a CHI transaction such as `ReadShared` from L2
  to HN-F. A single CPU request may create several protocol messages, snoops,
  data responses, retries, or writebacks.
- Individual transaction records are not retained. `ProtocolTrace` can print
  completion lines, but there is no structured per-request CSV/JSON trace.
- Exact min/max are not first-class sequencer stats. Histogram output provides
  samples, mean, stdev, and bucket information; exact observed minimum and
  maximum are not reliable from bucketed histogram output alone.

## Existing Tool 2: CHI Controller Transaction Stats

Relevant code:

- `src/mem/ruby/slicc_interface/AbstractController.hh`
- `src/mem/ruby/slicc_interface/AbstractController.cc`
- `src/mem/slicc/symbols/StateMachine.py`
- `src/mem/ruby/protocol/chi/CHI-cache.sm`
- `src/mem/ruby/protocol/chi/CHI-cache-actions.sm`
- `src/mem/ruby/protocol/chi/CHI-dvm-misc-node.sm`
- `src/mem/ruby/protocol/chi/CHI-dvm-misc-node-actions.sm`

The CHI SLICC protocol marks many events with `in_trans="yes"` and
`out_trans="yes"`. `StateMachine.py` generates stats for those events:

```text
inTransLatHist.<Event>
outTransLatHist.<Event>
inTransLatHist.<Event>.<InitialState>.<FinalState>.total
inTransLatHist.<Event>.retries
outTransLatHist.<Event>.retries
```

The runtime hooks live in `AbstractController`:

- `incomingTransactionStart(addr, event, initialState, retried, isAddressed)`
- `incomingTransactionEnd(addr, finalState, isAddressed)`
- `outgoingTransactionStart(addr, event, isAddressed)`
- `outgoingTransactionEnd(addr, retried, isAddressed)`

The CHI cache machine already calls these in `CHI-cache-actions.sm`. The
`CHI-cache.sm` event list marks many useful incoming transactions, including:

```text
Load, Store, Prefetch,
ReadShared, ReadNotSharedDirty, ReadUnique, ReadNoSnp, ReadOnce,
CleanUnique, MakeReadUnique,
Evict, WriteBackFull, WriteEvictFull, WriteCleanFull,
WriteUnique, WriteUniqueFull_PoC, WriteUniqueZero,
AtomicReturn, AtomicNoReturn,
SnpShared, SnpUnique, SnpOnce, SnpCleanInvalid,
Local_Eviction, LocalHN_Eviction, Global_Eviction
```

The outgoing transaction events include:

```text
SendReadShared
SendReadOnce
SendReadNoSnp
SendReadNoSnpDMT
SendReadUnique
SendMakeReadUnique
SendWriteBackOrWriteEvict
SendWriteClean
SendWriteNoSnp
SendWriteNoSnpPartial
SendWriteUnique
SendAtomicReturn
SendAtomicNoReturn
SendEvict
SendCleanUnique
```

Use this when the question is protocol/controller-local:

- "For this L2/RN-F, how long do `SendReadShared` outgoing transactions take?"
- "For this HN-F, how long does an incoming `ReadShared` transaction take from
  request arrival until the home-side state machine finishes it?"
- "What states did `ReadShared` start/end in?"
- "How many retried CHI transactions of this event were seen?"

Important interpretation:

- `inTransLatHist.ReadShared` on an HN-F measures the controller's incoming
  `ReadShared` transaction after that request has reached the HN-F, not the
  original CPU-visible latency.
- `outTransLatHist.SendReadShared` on an RN-F/L2 measures the local outgoing
  transaction window started when the controller sends the request and ended
  when the CHI action calls `outgoingTransactionEnd()`. This is closer to
  "L2 sends `ReadShared` to Home, how long until that transaction completes?"
- The stat is by controller SimObject. In `stats.txt`, look for the concrete
  object name of the CHI cache controller, then the `inTransLatHist.*` or
  `outTransLatHist.*` stat under it.

Limitations:

- The start/end map key is only `Addr` for addressed transactions or a unique ID
  for unaddressed DVM transactions. This prevents two concurrent outgoing
  transactions for the same line in the same controller; the current code asserts
  if that happens. It is consistent with the current protocol model, but it is
  not a general transaction-ID trace.
- The stats are histograms. They are good for count/mean/distribution shape, but
  exact per-event min/max should be added explicitly if needed.
- The stats do not correlate the RN-F outgoing transaction with the HN-F
  incoming transaction as one end-to-end CHI transaction. You can compare the two
  views statistically, but you cannot reconstruct an individual request path
  without a trace/correlation ID.
- Only events marked `in_trans` or `out_trans` get generated stats. If a CHI
  event you care about is not annotated, add the annotation and the start/end
  calls in the relevant action path.

## Existing Tool 3: Ruby Message Delay and MessageBuffer Stats

Relevant code:

- `src/mem/ruby/network/MessageBuffer.hh`
- `src/mem/ruby/network/MessageBuffer.cc`
- `src/mem/ruby/slicc_interface/Message.hh`
- `src/mem/ruby/slicc_interface/AbstractController.hh`
- `src/mem/ruby/slicc_interface/AbstractController.cc`

Every Ruby `Message` tracks:

- original construction time: `Message::getTime()`
- last enqueue time: `getLastEnqueueTime()`
- accumulated delayed ticks: `getDelayedTicks()`

`MessageBuffer::enqueue()` and `MessageBuffer::dequeue()` update those fields.
Message buffers also expose built-in stats:

```text
<buffer>.m_msg_count
<buffer>.m_buf_msgs
<buffer>.m_stall_time
<buffer>.m_stall_count
<buffer>.m_avg_stall_time
<buffer>.m_occupancy
```

Controllers can also sample delay histograms through
`AbstractController::profileMsgDelay(vnet, delay)`, and the global Ruby profiler
collates these as:

```text
system.ruby.delayHistogram...
system.ruby.delayVCHist.vnet_<n>...
```

As with the sequencer histograms, these are merged into the RubySystem group
(no `profiler` node) and carry the `nozero` flag, so they are omitted entirely
if no message delay was sampled. CHI rarely calls `profileMsgDelay()`, so on a
CHI run `delayHistogram`/`delayVCHist.*` are often empty even when traffic is
flowing.

Use this when the question is queueing/buffering:

- "Is a controller input/output queue accumulating stall time?"
- "Which virtual network is seeing message delay?"
- "Is mandatoryQueue pressure a likely source of endpoint latency?"

Limitations:

- Message delay is not the same as CHI transaction latency.
- `profileMsgDelay()` is protocol-action dependent. CHI does not use it as
  comprehensively as the CHI transaction hooks.
- MessageBuffer stats do not identify CHI opcodes or correlate a request with
  its eventual completion.

## Existing Tool 4: Garnet Network Packet and Flit Latency

Relevant code:

- `src/mem/ruby/network/garnet/GarnetNetwork.hh`
- `src/mem/ruby/network/garnet/GarnetNetwork.cc`
- `src/mem/ruby/network/garnet/NetworkInterface.cc`
- `src/mem/ruby/network/garnet/flit.hh`
- `src/mem/ruby/network/garnet/flit.cc`

Garnet has built-in packet/flit counters and average latency formulas:

```text
system.ruby.network.packets_injected
system.ruby.network.packets_received
system.ruby.network.packet_network_latency
system.ruby.network.packet_queueing_latency
system.ruby.network.average_packet_vnet_latency
system.ruby.network.average_packet_vqueue_latency
system.ruby.network.average_packet_network_latency
system.ruby.network.average_packet_queueing_latency
system.ruby.network.average_packet_latency

system.ruby.network.flits_injected
system.ruby.network.flits_received
system.ruby.network.flit_network_latency
system.ruby.network.flit_queueing_latency
system.ruby.network.average_flit_vnet_latency
system.ruby.network.average_flit_vqueue_latency
system.ruby.network.average_flit_network_latency
system.ruby.network.average_flit_queueing_latency
system.ruby.network.average_flit_latency
system.ruby.network.average_hops
```

`NetworkInterface::incrementStats()` splits Garnet latency into:

- `network_delay`: time from flit injection into Garnet to dequeue from the
  destination NI, minus one cycle.
- `src_queueing_delay`: time spent waiting in the source Ruby/network interface
  before flitization.
- `dest_queueing_delay`: time from destination NI dequeue to insertion into the
  destination protocol buffer.
- packet latency is sampled only on the tail or head-tail flit.

Use this when the question is interconnect behavior:

- "What is the average Ruby network packet latency?"
- "How much is network traversal versus NI/protocol-buffer queueing?"
- "Which vnet carries the higher latency?"

Limitations:

- Garnet only exposes sums and averages by vnet for packet/flit latency, not
  histograms or exact min/max.
- It is not grouped by CHI opcode, source controller, destination controller, or
  RN-F/HN-F role.
- Garnet packet ID exists internally (`flit::getPacketID()`), but it is not
  dumped as a per-packet trace by default.

## Existing Tool 5: SimpleNetwork Stats and Route Profiler

Relevant code:

- `src/mem/ruby/network/simple/SimpleNetwork.hh`
- `src/mem/ruby/network/simple/SimpleNetwork.cc`
- `src/mem/ruby/network/RouteProfiler.hh`
- `src/mem/ruby/network/RouteProfiler.cc`
- `configs/network/Network.py`

SimpleNetwork provides message counts/bytes by `MessageSizeType`:

```text
system.ruby.network.msg_count.<MessageSizeType>
system.ruby.network.msg_byte.<MessageSizeType>
```

Each `MessageBuffer` used by the simple network also has buffer occupancy and
stall stats as described above.

There is also a 2026 route profiler enabled by:

```text
--network=simple --simple-trace-routes
```

It dumps `interconnect_routes.txt` at exit. Each line contains average route
delay, count, vnet, source link, routers with frontend/backend average delay,
and destination link. This is useful for finding bad routes or congested switch
stages.

Limitations:

- SimpleNetwork currently does not expose a normal packet/message latency
  histogram comparable to Garnet's average packet latency.
- The route profiler aggregates by route and does not report CHI opcode,
  controller event, exact min/max, or per-individual transaction records.

## Individual Transaction-Level Debugging

Current debug flags can help:

```text
--debug-flags=ProtocolTrace,RubySequencer,RubyGenerated,RubySlicc,RubyQueue,RubyNetwork
```

Useful narrower choices:

- `ProtocolTrace`: sequencer completion lines include address and total cycles
  in `Sequencer::recordMissLatency()`.
- `RubySequencer`: request insertion/callback behavior.
- `RubyGenerated` and `RubySlicc`: generated controller transitions.
- `RubyQueue`: message buffer enqueue/dequeue/stall behavior.
- `RubyNetwork`: Garnet flit/network behavior.

This is enough for manual debugging of a small test, especially with a single
address and one outstanding transaction. It is not enough for scalable analysis:
debug logs are text-heavy, not structured, and do not provide a stable
transaction ID that follows an RN-F request through HN-F, snoops, data responses,
and completion.

## How to Answer Common Questions Today

### "Request from sequencer mandatory queue: how long until completion?"

Use sequencer/profiler stats (note: directly under `system.ruby`, with no
`profiler` node — see "Existing Tool 1"):

```text
system.ruby.m_latencyHistSeqr
system.ruby.RequestType.<type>.latency_hist_seqr
system.ruby.m_hitLatencyHistSeqr
system.ruby.m_missLatencyHistSeqr
```

For individual inspection, enable `ProtocolTrace` and filter for the address.
The structured statistical answer exists; the structured individual answer needs
a trace patch.

### "L2 sends ReadShared to Home: how long until completion?"

Use the CHI controller outgoing transaction stat on the L2/RN-F controller:

```text
<l2_or_rnf_controller>.outTransLatHist.SendReadShared
<l2_or_rnf_controller>.outTransLatHist.SendReadShared.retries
```

This is the most direct existing stat. It measures from the CHI action that
starts the outgoing transaction to the action that ends it.

To understand the home-side part, use the HN-F incoming transaction stat:

```text
<hnf_controller>.inTransLatHist.ReadShared
<hnf_controller>.inTransLatHist.ReadShared.<InitialState>.<FinalState>.total
<hnf_controller>.inTransLatHist.ReadShared.retries
```

These two numbers are related but not the same. The RN-F outgoing latency
includes network round trip and remote processing needed for local completion.
The HN-F incoming latency starts only after the request arrives at the home.

### "Average Packet latency processing in Ruby?"

For Garnet:

```text
system.ruby.network.average_packet_latency
system.ruby.network.average_packet_network_latency
system.ruby.network.average_packet_queueing_latency
system.ruby.network.average_packet_vnet_latency
system.ruby.network.average_packet_vqueue_latency
```

For SimpleNetwork:

- There is no direct average packet latency stat.
- Use `--simple-trace-routes` for route-level average delays.
- Use `MessageBuffer` stall/occupancy stats to diagnose queueing.
- Add a SimpleNetwork latency patch if you need packet/message latency
  comparable to Garnet.

### "Min/max/avg latency for L2 Cache controller in Tile 0?"

Average is available from that controller's CHI transaction histograms. Exact
min/max are not cleanly available today from the existing histogram output. You
should add explicit min/max tracking next to:

```text
<tile0_l2_controller>.outTransLatHist.<Event>
<tile0_l2_controller>.inTransLatHist.<Event>
```

If the question means CPU-visible requests from that tile's sequencer, add
per-sequencer stats rather than relying only on the global Ruby profiler
collation.

## Recommended Patches

### Patch 1: Exact Min/Max/Avg Transaction Stats Per Controller

Add a small stats helper to `AbstractController::ControllerStats` for each
`in_trans` and `out_trans` event:

- samples/count
- total cycles
- min cycles
- max cycles
- average formula

Keep the existing histograms. The new stats make min/max exact and easy to grep.
Update them in:

- `AbstractController::incomingTransactionEnd()`
- `AbstractController::outgoingTransactionEnd()`

This is low risk and immediately answers:

- min/max/avg `SendReadShared` per L2/RN-F
- min/max/avg `ReadShared` per HN-F
- retry counts already exist and can remain as they are

### Patch 2: Structured CHI Transaction Trace

Add an optional trace facility, disabled by default, that emits one row per
transaction end:

```text
tick_start,tick_end,cycles,controller,event,direction,addr_or_txn_id,
initial_state,final_state,retried,is_addressed,machine_type,version
```

Good implementation points:

- `AbstractController::incomingTransactionStart/End()`
- `AbstractController::outgoingTransactionStart/End()`

This gives individual transaction-level answers without parsing debug text.
Prefer a file or probe point over a debug flag if the output will be consumed by
scripts. A debug flag can still be useful for ad hoc inspection.

### Patch 3: Correlation ID Across RN-F, Network, and HN-F

If you need to reconstruct one end-to-end CHI transaction, extend the CHI
message/TBE path with a correlation ID. The key should survive:

- original sequencer request
- RN-F outgoing CHI request
- Garnet/SimpleNetwork message
- HN-F incoming request
- snoops and data responses where applicable
- RN-F completion

Possible sources:

- a monotonically increasing per-controller transaction sequence number
- CHI protocol transaction ID where modeled
- address plus controller plus local sequence number

Do not rely on address alone for long-term tracing. Address-only maps are fine
for current protocol stats but are weak for trace correlation and postprocessing.

### Patch 4: Per-Sequencer Endpoint Latency Stats (CPU Sequencers)

**Goal:** dump latency stats separately for each CPU `Sequencer`, so that
"Tile 0 only" or "core 3 only" questions can be answered directly from
`stats.txt`.

**Why it is not available today.** `Sequencer` derives from `RubyPort` ->
`ClockedObject` -> `SimObject`, so every sequencer already has its own object
path in `stats.txt` (for example `system.cpu0.l1d.sequencer`). The latency
data is also already collected per sequencer: `recordMissLatency()` fills
`m_latencyHist`, `m_hitLatencyHist`, `m_missLatencyHist`, and the
per-type/per-machine vectors for each sequencer individually. The only thing
missing is stat *registration*. In `Sequencer.cc` these histograms are
constructed bare:

```cpp
statistics::Histogram m_latencyHist;            // member, no name, no group
m_latencyHist.init(10);                         // only init() is called
m_typeLatencyHist.push_back(new statistics::Histogram());  // no parent
```

With no name and no parent `statistics::Group`, they are never emitted under
the sequencer SimObject. `Profiler::collateStats()` only sums them into the
merged, system-wide `system.ruby.*` stats.

**The fix is registration only — no change to the recording path.** Give the
`Sequencer` a nested `statistics::Group` (or attach the histograms directly to
the `Sequencer`, which is itself a `Group`) and give every histogram a name
and description:

```cpp
// In Sequencer.cc, instead of bare members / new Histogram():
m_latencyHist
    .init(10)
    .name(name() + ".latency_hist")
    .desc("latency of all requests handled by this sequencer")
    .flags(statistics::nozero | statistics::pdf | statistics::oneline);

m_typeLatencyHist[i] = new statistics::Histogram(this);  // 'this' = parent Group
m_typeLatencyHist[i]
    ->init(10)
    .name(csprintf("latency_hist.%s", RubyRequestType(i)))
    .desc("")
    .flags(statistics::nozero | statistics::pdf | statistics::oneline);
```

Result — one set of histograms per sequencer, under its own path:

```text
system.cpu0.l1d.sequencer.latency_hist
system.cpu0.l1d.sequencer.hit_latency_hist
system.cpu0.l1d.sequencer.miss_latency_hist
system.cpu0.l1d.sequencer.outstanding_req_hist
system.cpu0.l1d.sequencer.latency_hist.<RubyRequestType>
```

Optionally add scalar min/max/avg updated directly in `recordMissLatency()`
for exact extremes:

```text
system.cpu0.l1d.sequencer.request_latency_min
system.cpu0.l1d.sequencer.request_latency_max
system.cpu0.l1d.sequencer.request_latency_avg
```

Because the `Sequencer` is a `SimObject`, these appear under its own object
path automatically — no `Profiler` collation needed, and no merge into
`system.ruby`. Keep the existing collated `system.ruby.m_latencyHistSeqr` etc.
for the whole-system view. This is a small, low-risk change: the histograms,
their `init(10)` sizing, the `sample()` calls in `recordMissLatency()`, and
`Sequencer::resetStats()` already exist.

Note: `Sequencer::collateStats()` is *declared* in `Sequencer.hh` but has no
definition and is never called. It is dead leftover, not a usable hook; ignore
it.

### Patch 4b: DMA Sequencer Latency Stats

**Goal:** dump latency stats for each `DMASequencer`, which the CPU-sequencer
profiler never covers.

**Why it is harder than Patch 4.** `DMASequencer` is also a `RubyPort`
SimObject, so per-object stats are possible — but, unlike the CPU `Sequencer`,
it has *no latency instrumentation at all*. `DMARequest` only carries
`bytes_completed` and `bytes_issued`; there is no issue timestamp and no
histogram. `Profiler::collateStats()` also skips it on purpose:
`getCPUSequencer()` returns NULL for it, and `getGPUCoalescer()` does not match
it either. So there is currently nothing to dump.

Adding it requires real new code, not just registration:

1. **Record an issue time.** Add a `Tick` (or `Cycles`) issue timestamp to
   `DMARequest`, set when the request is created/enqueued in
   `DMASequencer::makeRequest()`.
2. **Sample latency on completion.** In the DMA completion path
   (`DMASequencer::issueNext()` when the request finishes, and the
   atomic/`ruby_hit_callback` path), compute `curTick() - issue_time` and
   sample it.
3. **Register named stats** on the `DMASequencer` SimObject — a
   `statistics::Histogram` and/or scalar min/max/avg, named and parented to the
   `DMASequencer` (which is a `Group`):

```text
system.<dma>.dma_sequencer.request_latency_hist
system.<dma>.dma_sequencer.request_latency_min
system.<dma>.dma_sequencer.request_latency_max
system.<dma>.dma_sequencer.request_latency_avg
system.<dma>.dma_sequencer.bytes_completed
```

The same approach applies to any non-CPU, non-DMA sequencer port
(`is_cpu_sequencer=False`): it is a SimObject, so per-object stats are
possible, but you must add both the measurement and the stat registration.

**Summary of effort:**

| Sequencer kind | Latency data collected today? | Work to dump per-object stats |
| --- | --- | --- |
| CPU `Sequencer` | Yes — full histograms, per object | Small: name + group the existing histograms (Patch 4) |
| `DMASequencer`   | No — only byte counts | Larger: add issue-time tracking + sampling + stats (Patch 4b) |

### Patch 5: Network Latency Histograms and Min/Max

For Garnet, add packet/flit histograms and exact min/max next to the existing
average stats:

- per-vnet packet network latency
- per-vnet packet queueing latency
- per-vnet total packet latency
- same for flits if needed

Update in `NetworkInterface::incrementStats()`.

For SimpleNetwork, add message latency sampling at route completion or endpoint
delivery. Reuse `Message::getTime()` or add an explicit network injection time,
then sample:

- source queueing
- switch/link traversal
- destination queueing
- total message network latency

The existing route profiler is a good diagnostic, but a normal stats path is
needed for min/max/avg packet latency.

### Patch 6: Optional Opcode/Controller Dimensions for Network Stats

If you need "ReadShared packet latency from RN-F 0 to HN-F 3", add optional
classification to network stats:

- source controller MachineID/version
- destination MachineID/version
- vnet
- CHI opcode/event or message type

Be careful with stat cardinality. For large systems, prefer structured trace
output and postprocess it rather than creating thousands of gem5 stats.

## Suggested Workflow

1. Start with stats:

   ```text
   --network=garnet
   ```

   Then inspect `stats.txt` for `average_packet_latency`,
   `outTransLatHist.SendReadShared`, and `inTransLatHist.ReadShared`.

2. For SimpleNetwork route diagnosis, add:

   ```text
   --network=simple --simple-trace-routes
   ```

   Then inspect `interconnect_routes.txt`.

3. For a tiny reproducer, enable:

   ```text
   --debug-flags=ProtocolTrace,RubySequencer,RubyGenerated,RubyQueue,RubyNetwork
   ```

   Filter by address and controller name.

4. For production-quality answers, implement Patch 1 and Patch 2 first. They
   are the highest-value changes because they provide exact min/max/avg and
   individual transaction rows while reusing the CHI transaction hooks that
   already exist.

## Bottom Line

The current codebase already has the right conceptual hooks for CHI transaction
latency. `in_trans`/`out_trans` stats are the key feature for RN-F/HN-F protocol
transactions, and sequencer histograms cover CPU-visible Ruby request latency.
Garnet also exposes useful network averages.

The missing pieces are exact min/max, structured individual transaction traces,
and correlation across controllers/network messages. Those can be added with
small, well-contained patches around `AbstractController`, `Sequencer`, and
`NetworkInterface`, without redesigning CHI itself.
