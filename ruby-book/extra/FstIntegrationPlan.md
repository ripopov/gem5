# Plan: Integrate libfst Waveform Tracing into gem5

## Implementation Status

| Item | Status | Commits |
|------|--------|---------|
| **libfst vendored + unit tests** | Done | `13256b32ae` |
| **Stage 1: Event tracing** | Done | `cd9f49113e` |
| **Stage 1: Traffic trace regression** | Done | `3689d27c2a` |
| **Stage 1: Clock counters** | Done | `a8f8ee91bf` |
| **Stage 2: Stat sampling (plan)** | Done | `655c17720c` |
| **Deprecation plan** | Done | `ad3a32e811` |
| **Deprecate ownerless constructors** | Done | `349953cb71` — `[[deprecated]]` on `EventFunctionWrapper` and `MemberEventWrapper` ownerless ctors |
| **P1 migration: memory + Ruby** | Done | `349953cb71` — xbar, simple\_mem, dram\_interface, nvm\_interface, hbm\_ctrl, bridge, packet\_queue, comm\_monitor, Consumer, Sequencer, RubySystem |
| **P2 migration: CPU + sim core** | Done | `091925be9b` — base CPU, atomic, timing, minor, O3, LSQ, TraceCPU, Root, Ticked |
| **P3 migration: RISC-V devices** | Done | `f978b90713` — PLIC, LupioBLK, LupioTMR, Uart8250, DmaPort, CopyEngine, IdeDisk |
| **Stage 3: Probe listeners** | Not started | — |

### Remaining ownerless call sites

The following still use deprecated ownerless constructors (outside the RISC-V build path or lacking SimObject access):

- `DmaCallback::getChunkEvent()` (`src/dev/dma_device.hh`) — no SimObject ref available, used by GPU/ARM `DmaVirtDevice`
- `src/mem/cache/compressors/frequent_values.cc` — 1 ownerless call
- ARM devices (`src/dev/arm/*`) — ~25 members across ~15 files
- AMD GPU (`src/dev/amdgpu/*`, `src/gpu-compute/*`) — ~5 files
- x86 (`src/arch/x86/*`) — ~2 files
- MIPS (`src/arch/mips/*`) — ~1 file
- Network devices (`src/dev/net/*`) — ~6 files
- SystemC bridges (`src/systemc/*`) — ~1 file
- Learning gem5 tutorials (`src/learning_gem5/*`) — ~2 files

These produce deprecation warnings only when building their respective ISA targets.

---

## Context

gem5 has no built-in waveform dump capability.
Hardware designers are accustomed to viewing simulation results in waveform viewers (GTKWave, Surfer).
The libfst library is already vendored at `ext/libfst/` with full writer/reader support.
The goal is to create an `FstTrace` SimObject that maps the SimObject hierarchy to an FST scope tree
and records dispatches of statically owned SimObject events during simulation.

Unlike RTL simulators that toggle clock signals every cycle, gem5 is event-driven:
computation happens only when events fire.
Rather than synthesizing fake clock toggles, we trace **actual event dispatches** —
each event becomes an `FST_VT_VCD_EVENT` signal inside its hosting SimObject's FST scope.

The implementation is split into three stages, each independently testable.

---

## Core Mechanism: Explicit Event Ownership + Streaming FST Writes

Events in gem5 that are useful for waveform tracing are predominantly constructed at
elaboration time and are logically owned by SimObjects. Instead of trying to infer
ownership from `Event::name()`, V1 requires statically created events to be explicitly
owned by a `SimObject` and creates FST signals upfront before simulation starts.

### Explicit event ownership

Keep ownership metadata outside `Event` to preserve the hot object layout.
Use a side registry such as:
- `static std::unordered_map<const Event*, const SimObject*> staticEventOwners`

For statically created events, require wrappers and event types to pass the owning
`SimObject` as the first constructor argument.
Examples:
- `MemberEventWrapper` takes `SimObject &owner` as the first constructor argument
- `EventFunctionWrapper` takes `const SimObject &owner` as the first constructor argument

At `startup()`, `FstTrace` walks the registry of statically created events,
looks up each event in `staticEventOwners`, groups them by owner, and creates FST
signals from the event's standard `name()`.
For FST compatibility, the emitted signal name is derived from `name()` by replacing
spaces and dots with `_`.

### Event registry

Keep a static `std::vector<Event*> allEvents` in `Event`.
Constructor pushes `this`; destructor removes it.
This registry is only used to enumerate candidate events at startup.
Ownership comes from the side registry populated by explicit owner-aware construction,
not from parsing `Event::name()`.

### Streaming FST writes

Since all signals are known at `startup()`, we open the FST file, write the hierarchy,
and emit value changes in real time via the dispatch callback.
No buffering, no deferred writes.

### Dynamic events

Events created after `startup()` are not supported in V1.
FST requires the design hierarchy and signals to be declared before streaming value
changes, so dynamic events cannot be represented cleanly without a different trace model.
This plan therefore scopes Stage 1 to **static SimObject event tracing** only.

### Dispatch hook

Add a raw function pointer + opaque context on `EventQueue`:

```cpp
// In EventQueue:
void (*dispatchHook)(const Event*, void*) = nullptr;
void* dispatchHookArg = nullptr;
```

In `serviceOne()`, call it before `event->process()`:

```cpp
if (dispatchHook)
    dispatchHook(event, dispatchHookArg);
```

When null, cost is a single pointer comparison.
No `std::function` (avoids heap allocation and indirect vtable call on the hot path).

Install this hook on every active `EventQueue`, not just queue 0.
`FstTrace` owns the global trace state, while each queue callback forwards the currently
dispatched event and queue-local time into the same FST writer.

### Event → FST handle mapping

`FstTrace` owns an `std::unordered_map<const Event*, fstHandle>` built at `startup()`.
The dispatch hook callback does an O(1) hash lookup per event dispatch.
Events not in the map (dynamic events, unresolved) are skipped.

This avoids adding any fields to the `Event` class itself, preserving its
cache-optimized layout. The hash lookup (~20–30 ns) is negligible relative to
`process()` cost. If profiling shows otherwise, the map can be swapped for a
faster open-addressing implementation (e.g. `absl::flat_hash_map`) without
changing any interfaces.

---

## Directory Layout

```
src/sim/fst_trace/
    FstTrace.py              # Python SimObject definition
    fst_trace.hh             # C++ header
    fst_trace.cc             # C++ implementation
    fst_trace_hier.test.cc   # GTest: hierarchy + event signal round-trips
    SConscript               # Build rules

tests/gem5/fst_trace/
    test_fst_trace.py        # Python test runner
    configs/
        fst_events.py        # Minimal system: traffic gen → crossbar → simple memory
        fst_stats.py         # Same system + stat sampling (Stage 2)
        fst_probes.py        # Same system + probe bindings (Stage 3)
```

---

## Stage 1: Hierarchy Dump + Static Event Tracing

**Goal:** Register statically owned SimObject events, walk SimObject hierarchy,
create FST scope tree with per-event signals, and stream event dispatches during
simulation.

### 1.1 Event Registration + Explicit Ownership

Modify `Event` class (`src/sim/eventq.hh`, `src/sim/eventq.cc`):
- Add `static std::vector<Event*> allEvents`
- Constructor: `allEvents.push_back(this)`
- Destructor: erase from `allEvents`

Add side storage for static event ownership outside `Event`:
- `static std::unordered_map<const Event*, const SimObject*> staticEventOwners`
- owner-aware constructors register into this map
- destructors erase from this map

For static events, require explicit owner-aware construction:
- `MemberEventWrapper` takes the owning `SimObject` as the first constructor argument and registers itself in `staticEventOwners`
- `EventFunctionWrapper` takes the owning `SimObject` as the first constructor argument and registers itself in `staticEventOwners`
- statically declared custom events must pass their owning `SimObject` explicitly

This requirement is intentional: if a static event does not declare its owning
`SimObject`, the trace cannot be considered complete. V1 therefore makes owner-aware
construction mandatory for static events rather than optional.

### 1.2 SimObject Hierarchy → FST Scopes

- Add `static const vector<SimObject*> &getSimObjectList()` to SimObject
- Sort by `name()` (full dotted path like `system.cpu0.icache`)
- Track `vector<string> currentScope`; compute push/pop per object:
  - `fstWriterSetScope(ctx, FST_ST_VCD_MODULE, component, NULL)` per new level
  - `fstWriterSetUpscope(ctx)` per level to pop
- Within each scope, create an `FST_VT_VCD_EVENT` variable per explicitly owned static event belonging to that SimObject
- Signal names come from `event->name()` after replacing spaces and dots with `_`

### 1.3 FST File Lifecycle

| gem5 Phase | Action |
|------------|--------|
| Constructor | Store params, register exit callback for `fstWriterClose()` |
| `init()` | Collect and sort SimObjects for hierarchy emission |
| `startup()` | Walk `Event::allEvents`, require explicit ownership for static events, open FST file, write hierarchy + signals, install dispatch hooks on all EventQueues, set `dumpActive` from `start_active` param |
| Simulation | Dispatch hook checks `dumpActive` flag, emits event toggles to FST just before functional event processing; Python or ROI markers can toggle tracing |
| Exit callback | Close FST file |

All `fstWriterCreateVar()` calls complete in `startup()` before any value changes.

### 1.4 Dispatch Hook

At `startup()`, install a static trampoline on every active `EventQueue`:

```cpp
// In startup():
for (auto *eventq : allMainEventQueues) {
    eventq->dispatchHook = &FstTrace::dispatchTrampoline;
    eventq->dispatchHookArg = this;
}

// Static callback — raw function pointer, no std::function overhead:
void FstTrace::dispatchTrampoline(const Event *event, void *arg) {
    auto *self = static_cast<FstTrace*>(arg);
    if (!self->dumpActive) return;                  // ROI blackout, skip
    auto it = self->eventHandleMap.find(event);
    if (it == self->eventHandleMap.end()) return;   // dynamic/unresolved, skip
    self->emitTimeChange(curTick());
    fstWriterEmitValueChange(self->fstCtx, it->second, "1");
}
```

The `FST_VT_VCD_EVENT` type is purpose-built for instantaneous occurrences —
waveform viewers render each value change as a marker rather than a level.
A single `"1"` emit per dispatch is sufficient; no `"1"` → `"0"` pair needed.

The hook must run after `setCurTick(event->when())` and after the squashed-event check,
but immediately before `event->process()`. That placement matches the intended meaning:
the event marker indicates that gem5 is about to execute the functional code for this event.

Because multiple event queues may advance at different rates, the callback must always
emit the current queue's `curTick()` before writing any value changes.
`FstTrace` should track the last timestamp written and only advance the FST stream when
the observed tick changes.

`eventHandleMap` is `std::unordered_map<const Event*, fstHandle>`, built once at
`startup()`. Ownership is resolved from the side registry, and signal names are
created from the event's standard `name()` with spaces and dots replaced by `_`.

### 1.5 ROI Tracing (Enable/Disable)

FST natively supports **blackout regions** via `fstWriterEmitDumpActive(ctx, enable)`.
When dump is inactive, waveform viewers show an empty gap — no signals, no toggles.
This is the standard mechanism used by `$dumpoff` / `$dumpon` in Verilog.

**C++ interface:**

```cpp
// In FstTrace:
bool dumpActive = true;  // starts enabled

void FstTrace::setDumpActive(bool enable) {
    if (enable == dumpActive) return;
    dumpActive = enable;
    fstWriterEmitTimeChange(fstCtx, curTick());
    fstWriterEmitDumpActive(fstCtx, enable ? 1 : 0);
    // On re-enable, clear hasEmitted so the next stat sample emits all
    // values unconditionally (viewers treat blackout exit as unknown state).
    if (enable)
        std::fill(hasEmitted.begin(), hasEmitted.end(), false);
}
```

The dispatch hook checks `dumpActive` as the very first thing (before the hash
lookup), so disabled regions have minimal overhead — a single bool check per dispatch.

**Python interface:**

The `FstTrace` SimObject exposes `setDumpActive()` to Python via pybind11,
allowing simulation scripts to toggle tracing dynamically:

```python
# In simulation script — trace only the ROI:
fst = FstTrace()
# ... build system ...
m5.simulate(warmup_ticks)
fst.setDumpActive(True)     # begin ROI
m5.simulate(roi_ticks)
fst.setDumpActive(False)    # end ROI
m5.simulate(cooldown_ticks)
```

**Initial state parameter:**

```python
class FstTrace(SimObject):
    # ...
    start_active = Param.Bool(True, "Start with dump enabled (False = start in blackout)")
```

Setting `start_active = False` lets the script start in blackout and only enable
tracing when the ROI begins. This avoids capturing warmup traffic.

**ROI marker integration:**

In addition to explicit Python control, `FstTrace` should support optional binding to
existing gem5 ROI mechanisms such as workload markers or magic instructions
(`m5_work_begin` / `m5_work_end`).
That keeps tracing control aligned with real workloads and avoids requiring scripts to
split every ROI into separate `simulate()` calls.

### 1.6 Stage 1 Tests

**GTest** (`src/sim/fst_trace/fst_trace_hier.test.cc`, `skip_lib=True`):
- Test hierarchy push/pop algorithm: feed sorted dotted names into scope builder,
  write to FST, read back with `fstReaderIterateHier()`, assert scope tree matches.
- Test event signals: create `FST_VT_VCD_EVENT` vars, emit values at known times,
  read back with `fstReaderIterBlocks()`, verify values + timestamps.
- Test blackout regions: emit events, call `fstWriterEmitDumpActive(ctx, 0)`,
  emit more events, call `fstWriterEmitDumpActive(ctx, 1)`, read back and verify
  blackout count and timestamps via `fstReaderGetNumberDumpActivityChanges()`.

**Python system test** (`tests/gem5/fst_trace/configs/fst_events.py`):
- Minimal config: traffic generator → crossbar → simple memory.
- Attach `FstTrace()`.
- Run for a small number of ticks.
- Post-check: verify scope hierarchy and event signals with FST reader or `fst2vcd`.
- Test ROI: run with `start_active=False`, enable mid-simulation via
  `setDumpActive(True)`, disable again, verify blackout regions in FST output.

---

## Stage 2: Periodic Stat Sampling

**Goal:** Auto-discover the entire gem5 stat hierarchy, mirror it as a dedicated
FST scope tree, and periodically sample every stat during simulation.

### 2.1 Design Overview

No user-specified stat bindings. At `startup()`, `FstTrace` recursively walks
the `statistics::Group` tree starting from `Root::root()`, creates a parallel
FST scope tree under a top-level `stats` scope, and creates FST signals for
every stat it encounters. A periodic event samples all registered stats and
emits value changes to the FST file.

The resulting FST hierarchy mirrors `stats.txt` exactly:
```
stats                          (top-level scope)
  system                       (Group scope)
    cpu0                       (Group scope)
      numCycles                (FST_VT_VCD_REAL)
      numInsts                 (FST_VT_VCD_REAL)
      committedInstsPerCycle   (FST_VT_VCD_REAL, from Formula)
      ...
    mem_ctrl                   (Group scope)
      bytesRead                (FST_VT_VCD_REAL)
      ...
```

This is controlled by a single parameter — the sampling period.
A period of 0 disables stat sampling entirely (pure Stage 1 event tracing).

### 2.2 Stat Type → FST Signal Mapping

The `Group::getStats()` method returns `Info*` pointers. Each `Info` subclass
maps to FST signals differently:

| Info subclass | FST representation |
|---|---|
| **ScalarInfo** | Single `FST_VT_VCD_REAL` signal — call `result()` |
| **VectorInfo** | One `FST_VT_VCD_REAL` per element, named from `subnames[i]` if available, else `_<i>`. Plus a `_total` signal from `total()`. |
| **FormulaInfo** | Same as VectorInfo (FormulaInfo inherits VectorInfo). |
| **DistInfo** | Two summary signals: `_mean` (computed as `sum / samples`) and `_samples`. |
| **Vector2dInfo** | One `FST_VT_VCD_REAL` per cell, named `_<x>_<y>`. |
| **VectorDistInfo** | Sub-scope per element, each with `_mean` and `_samples`. |
| **SparseHistInfo** | Single `_samples` signal (sparse map is dynamic, cannot pre-declare buckets). |

All signals use `FST_VT_VCD_REAL` (IEEE 754 double), which matches `Result`
(`typedef double Result`) exactly.

### 2.3 Hierarchy Walk and Signal Creation

At `startup()`, after Stage 1 event hierarchy is written, `FstTrace` builds
the stat scope tree:

```cpp
void FstTrace::createStatHierarchy()
{
    fstWriterSetScope(fstCtx, FST_ST_VCD_MODULE, "stats", NULL);
    walkStatGroup(Root::root());
    fstWriterSetUpscope(fstCtx);
}

void FstTrace::walkStatGroup(const statistics::Group *group)
{
    // Create signals for stats directly in this group
    for (auto *info : group->getStats()) {
        createStatSignals(info);
    }

    // Recurse into named child groups (creates FST scopes)
    for (auto &[name, child] : group->getStatGroups()) {
        fstWriterSetScope(fstCtx, FST_ST_VCD_MODULE, name.c_str(), NULL);
        walkStatGroup(child);
        fstWriterSetUpscope(fstCtx);
    }
}
```

`createStatSignals()` dispatches on Info subclass via `dynamic_cast`.

**Signal naming:** Since the walk already maintains the correct FST scope,
signal names must be the *leaf* name only (the last component after the final
`.` in `info->name`), not the full dotted path.  Using the full path would
produce redundant, unreadable names in GTKWave (e.g. `system_cpu0_numCycles`
inside the `system.cpu0` scope).  The helper `leafName()` extracts this:

```cpp
std::string leafName(const std::string &dotted)
{
    auto pos = dotted.rfind('.');
    return pos == std::string::npos ? dotted : dotted.substr(pos + 1);
}
```

**FormulaInfo before VectorInfo:** `FormulaInfo` inherits `VectorInfo`, so a
`dynamic_cast<VectorInfo*>` would match formulas too.  Check `FormulaInfo`
first to keep the dispatch explicit and order-independent.

```cpp
void FstTrace::createStatSignals(const statistics::Info *info)
{
    std::string leaf = leafName(info->name);

    if (auto *si = dynamic_cast<const statistics::ScalarInfo*>(info)) {
        fstHandle h = fstWriterCreateVar(fstCtx, FST_VT_VCD_REAL,
            FST_VD_OUTPUT, 64, sanitize(leaf).c_str(), 0);
        statHandles.push_back({h, info, StatKind::Scalar});
    }
    // FormulaInfo checked before VectorInfo (FormulaInfo inherits VectorInfo)
    else if (auto *fi = dynamic_cast<const statistics::FormulaInfo*>(info)) {
        for (size_t i = 0; i < fi->size(); ++i) {
            std::string ename = fi->subnames.size() > i
                    && !fi->subnames[i].empty()
                ? sanitize(fi->subnames[i])
                : sanitize(leaf) + "_" + std::to_string(i);
            fstHandle h = fstWriterCreateVar(fstCtx, FST_VT_VCD_REAL,
                FST_VD_OUTPUT, 64, ename.c_str(), 0);
            statHandles.push_back({h, info, StatKind::VectorElem, i});
        }
        fstHandle ht = fstWriterCreateVar(fstCtx, FST_VT_VCD_REAL,
            FST_VD_OUTPUT, 64,
            (sanitize(leaf) + "_total").c_str(), 0);
        statHandles.push_back({ht, info, StatKind::VectorTotal});
    }
    else if (auto *vi = dynamic_cast<const statistics::VectorInfo*>(info)) {
        for (size_t i = 0; i < vi->size(); ++i) {
            std::string ename = vi->subnames.size() > i
                    && !vi->subnames[i].empty()
                ? sanitize(vi->subnames[i])
                : sanitize(leaf) + "_" + std::to_string(i);
            fstHandle h = fstWriterCreateVar(fstCtx, FST_VT_VCD_REAL,
                FST_VD_OUTPUT, 64, ename.c_str(), 0);
            statHandles.push_back({h, info, StatKind::VectorElem, i});
        }
        fstHandle ht = fstWriterCreateVar(fstCtx, FST_VT_VCD_REAL,
            FST_VD_OUTPUT, 64,
            (sanitize(leaf) + "_total").c_str(), 0);
        statHandles.push_back({ht, info, StatKind::VectorTotal});
    }
    else if (auto *di = dynamic_cast<const statistics::DistInfo*>(info)) {
        fstHandle hm = fstWriterCreateVar(fstCtx, FST_VT_VCD_REAL,
            FST_VD_OUTPUT, 64,
            (sanitize(leaf) + "_mean").c_str(), 0);
        statHandles.push_back({hm, info, StatKind::DistMean});
        fstHandle hs = fstWriterCreateVar(fstCtx, FST_VT_VCD_REAL,
            FST_VD_OUTPUT, 64,
            (sanitize(leaf) + "_samples").c_str(), 0);
        statHandles.push_back({hs, info, StatKind::DistSamples});
    }
    // ... Vector2dInfo, VectorDistInfo, SparseHistInfo similarly
}
```

### 2.4 Periodic Sampling

`FstTrace` schedules a recurring `EventFunctionWrapper` at the configured
period:

```cpp
// In FstTrace:
EventFunctionWrapper sampleStatsEvent;  // owned by *this (FstTrace)
Tick statSamplePeriod;
std::vector<StatEntry> statHandles;   // {fstHandle, Info*, kind, index}
std::vector<bool> hasEmitted;         // parallel to statHandles, true after first write

void FstTrace::FstTrace(const Params &p)
    : SimObject(p),
      // sampleStatsEvent owned by *this so it appears in the FST event trace
      sampleStatsEvent(*this, [this] { sampleStats(); }, name()),
      ...
{}

void FstTrace::startup()
{
    // ... Stage 1 setup ...

    if (statSamplePeriod > 0) {
        createStatHierarchy();
        hasEmitted.resize(statHandles.size(), false);
        lastValues.resize(statHandles.size(), 0.0);
        schedule(sampleStatsEvent, curTick() + statSamplePeriod);
    }
}

void FstTrace::sampleStats()
{
    // Skip sampling during blackout — stat value changes inside FST
    // blackout regions would be silently discarded by viewers, and
    // re-emitting stale cumulative counters on re-enable is confusing.
    if (!dumpActive) {
        schedule(sampleStatsEvent, curTick() + statSamplePeriod);
        return;
    }

    // Recursively prepare all stats (formulas, averages recompute).
    // preDumpStats() walks child groups, but does NOT call prepare()
    // on individual Info objects.  We must do that ourselves via a
    // recursive walk that mirrors createStatHierarchy().
    Root::root()->preDumpStats();
    prepareStatsRecursive(Root::root());

    emitTimeChange(curTick());

    for (size_t i = 0; i < statHandles.size(); ++i) {
        double val = readStatValue(statHandles[i]);
        // Use hasEmitted flag instead of NaN sentinel.  NaN != NaN
        // is always true, so a NaN-initialized lastValues would
        // re-emit NaN stats every sample, defeating delta compression.
        if (!hasEmitted[i] || val != lastValues[i]) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%.15g", val);
            fstWriterEmitValueChange(fstCtx, statHandles[i].handle, buf);
            lastValues[i] = val;
            hasEmitted[i] = true;
        }
    }

    schedule(sampleStatsEvent, curTick() + statSamplePeriod);
}

// Recursively call prepare() on every Info in the Group tree.
void FstTrace::prepareStatsRecursive(statistics::Group *group)
{
    for (auto *info : group->getStats())
        info->prepare();

    for (auto &[name, child] : group->getStatGroups())
        prepareStatsRecursive(child);
}
```

`readStatValue()` dispatches on `StatKind`:

```cpp
double FstTrace::readStatValue(const StatEntry &entry)
{
    switch (entry.kind) {
      case StatKind::Scalar:
        return static_cast<const ScalarInfo*>(entry.info)->result();
      case StatKind::VectorElem:
        return static_cast<const VectorInfo*>(entry.info)->result()[entry.index];
      case StatKind::VectorTotal:
        return static_cast<const VectorInfo*>(entry.info)->total();
      case StatKind::DistMean: {
        auto *di = static_cast<const DistInfo*>(entry.info);
        return di->data.samples ? di->data.sum / di->data.samples : 0.0;
      }
      case StatKind::DistSamples:
        return static_cast<const DistInfo*>(entry.info)->data.samples;
      // ... other kinds
    }
}
```

**Delta-only writes:** Each signal tracks its last emitted value in
`lastValues`. Only changed values produce FST output, keeping files compact
when most stats are stable between samples.

**ROI interaction:** The sampling event always fires on schedule regardless of
`dumpActive`, but it skips emitting value changes when `dumpActive` is false.
FST blackout regions cause waveform viewers to render all signals as unknown —
writing stat values during a blackout would either be silently discarded or
produce confusing visual artifacts where stat traces appear inside a gap.
When `dumpActive` is re-enabled, the next sample will emit all values
unconditionally (delta detection treats the blackout exit as a fresh start
by clearing `hasEmitted`).

### 2.5 FST File Lifecycle (Updated for Stage 2)

| gem5 Phase | Action |
|------------|--------|
| Constructor | Store params, register exit callback for `fstWriterClose()` |
| `init()` | Collect and sort SimObjects for hierarchy emission |
| `startup()` | Open FST file; write Stage 1 event hierarchy + signals; if `stat_sample_period > 0`: walk `Group` tree, write `stats` scope tree + signals, schedule first sample event; install dispatch hooks |
| Simulation | Dispatch hook emits event markers (Stage 1); periodic sample event emits stat value changes when `dumpActive` is true (Stage 2) |
| Exit callback | Close FST file |

### 2.6 Stage 2 Tests

**GTest** (`src/sim/fst_trace/fst_trace_hier.test.cc`, `skip_lib=True`):
- Test real-valued signal round-trip: create `FST_VT_VCD_REAL` signals,
  emit known double values at known timestamps, read back with
  `fstReaderIterBlocks()`, verify values match within floating-point tolerance.
- Test delta-only encoding: emit same value twice at different times,
  verify only one value change appears in FST output.
- Test stat scope hierarchy: create nested scopes under `stats`,
  read back with `fstReaderIterateHier()`, assert scope tree is correct.

**Python system test** (`tests/gem5/fst_trace/configs/fst_stats.py`):
- Minimal config: traffic generator → crossbar → simple memory.
- Attach `FstTrace(stat_sample_period=1000)`.
- Run for a small number of ticks.
- Post-check: verify `stats` scope exists and contains expected groups
  (e.g., `stats.system.mem_ctrl`), stat signals have non-zero values,
  and samples are spaced at the configured period.

---

## Stage 3: Probe Point Listeners

**Status:** Future extension, not part of the current implementation scope.

**Goal:** Add `FstProbeBinding` configuration; connect typed listeners to probe points.

### 3.1 Design

- User configures `FstProbeBinding`: target SimObject, probe name, type, bit width
- In `regProbeListeners()`, create typed `ProbeListenerArgFunc<T>` per binding
- Lambda callback emits value change to FST signal
- Initial types: `uint64`, `bool`, `packet`

### 3.2 Stage 3 Tests

**GTest** (`skip_lib=True`): Test FST integer/multi-signal encoding round-trips.

**Python system test**: Reuse config, attach packet probe to crossbar, verify signals.

---

## Python Configuration Interface

```python
class FstTrace(SimObject):
    type = "FstTrace"
    trace_file = Param.String("trace.fst", "Output file name")
    compression = Param.String("lz4", "Compression: zlib, lz4, fastlz")
    timescale = Param.Int(-12, "FST timescale exponent (-12 = 1ps)")
    start_active = Param.Bool(True, "Start with dump enabled")
    use_work_item_roi = Param.Bool(False,
        "Toggle tracing from work item ROI markers when available")
    stat_sample_period = Param.Tick(0,
        "Stat sampling period in ticks (0 = disabled)")
```

---

## Build Integration

```python
# src/sim/fst_trace/SConscript
Import('*')
SimObject('FstTrace.py', sim_objects=['FstTrace'])
Source('fst_trace.cc')
DebugFlag('FstTrace')
GTest('fst_trace_hier.test', 'fst_trace_hier.test.cc', skip_lib=True)
```

---

## Files Modified (Stage 1) — All Done

| File | Change | Status |
|------|--------|--------|
| `src/sim/sim_object.hh` | Add `static const vector<SimObject*> &getSimObjectList()` | Done |
| `src/sim/eventq.hh` | Add `static vector<Event*> allEvents` in Event; add `dispatchHook` / `dispatchHookArg` raw pointers on EventQueue; `[[deprecated]]` on ownerless constructors | Done |
| `src/sim/eventq.cc` | Register/deregister in Event ctor/dtor; call `dispatchHook` in `serviceOne()`; side registry for static event ownership | Done |
| `src/sim/fst_trace/` | `FstTrace` SimObject: hierarchy dump, event dispatch tracing, clock counters, stat sampling | Done |
| Priority 1–3 migration | 29 files migrated to pass `SimObject&` owner to event constructors | Done |

## Files Modified (Stage 2) — Planned

| File | Change |
|------|--------|
| `src/sim/fst_trace/FstTrace.py` | Add `stat_sample_period = Param.Tick(0, ...)` |
| `src/sim/fst_trace/fst_trace.hh` | Add `sampleStatsEvent` (owned by `*this`), `statHandles`, `hasEmitted`, `lastValues`, `prepareStatsRecursive()`, `createStatHierarchy()`, `walkStatGroup()`, `createStatSignals()`, `sampleStats()`, `readStatValue()`, `leafName()` |
| `src/sim/fst_trace/fst_trace.cc` | Implement stat hierarchy walk, periodic sampling with blackout gating, recursive `prepare()`, `hasEmitted`-based delta detection |
| `src/sim/fst_trace/fst_trace_hier.test.cc` | Add GTests for real-valued signals, delta encoding, stat scope hierarchy |
| `tests/gem5/fst_trace/configs/fst_stats.py` | Python system test for stat sampling |

---

## Key Files to Reference

- `ext/libfst/fstapi.h` — FST writer/reader API (`fstWriterEmitDumpActive` for blackout regions)
- `src/sim/sim_object.hh:153` — `simObjectList` (private, need accessor)
- `src/sim/eventq.hh:407-420` — Event constructor (add self-registration here)
- `src/sim/eventq.cc:224-262` — `serviceOne()` dispatch point
- `src/sim/eventq.hh:1090-1177` — EventFunctionWrapper/MemberEventWrapper naming
- `src/base/output.hh:305` — `extern OutputDirectory simout`
- `src/sim/sim_exit.hh:45` — `registerExitCallback()`
- `src/base/fstapi.test.cc` — GTest pattern for FST round-trips
- `src/base/stats/group.hh` — `Group` class: `getStatGroups()`, `getStats()`, `resolveStat()`
- `src/base/stats/group.cc` — Group hierarchy walk, merged groups
- `src/base/stats/info.hh` — `ScalarInfo`, `VectorInfo`, `DistInfo`, `FormulaInfo`, etc.
- `src/base/statistics.hh` — Concrete stat types (`Scalar`, `Vector`, `Distribution`, `Formula`)
- `src/base/stats/text.cc` — Text dumper visitor pattern (reference for stat value extraction)
- `src/sim/root.hh:92` — `Root::root()` singleton (top of stat Group hierarchy)

---

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| **Dynamic events without FST signals** | Explicitly unsupported in Stage 1; scope is static SimObject event tracing only |
| **Missing event ownership metadata** | Make owning `SimObject` mandatory for static events; this is required so Stage 1 traces can be considered complete |
| **Multiple event queues / different local times** | Install hooks on all EventQueues; emit time changes from the current queue tick before value changes |
| **Dispatch hook overhead** | Raw function pointer: single null check when disabled; O(1) hash lookup when enabled |
| **Event destructor cost** | Linear scan of `allEvents` to erase; acceptable for rare destruction |
| **Thread safety** | `allEvents` populated before simulation (single-threaded); callbacks installed on all EventQueues |
| **Stat sampling overhead** | Delta-only writes (`hasEmitted` + `lastValues`) skip unchanged stats; `prepare()` cost is unavoidable but matches normal stat dump path |
| **Large number of stat signals** | Every stat gets an FST signal; file size mitigated by delta-only writes and FST compression (lz4) |
| **Stat `prepare()` ordering** | Call `preDumpStats()` on root group, then `prepareStatsRecursive()` to walk all sub-groups — `preDumpStats()` alone does not call `Info::prepare()` |
| **NaN stat values** | `hasEmitted` bitvector avoids NaN-sentinel comparison (`NaN != NaN` is always true, which would defeat delta compression) |
| **Stats during blackout** | Sampling event skips emission when `dumpActive` is false; `hasEmitted` is cleared on re-enable so first post-blackout sample emits all values |
