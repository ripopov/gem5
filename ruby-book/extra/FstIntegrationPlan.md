# Plan: Integrate libfst Waveform Tracing into gem5

## Context

gem5 has no built-in waveform dump capability.
Hardware designers are accustomed to viewing simulation results in waveform viewers (GTKWave, Surfer).
The libfst library is already vendored at `ext/libfst/` with full writer/reader support.
The goal is to create an `FstTrace` SimObject that maps the SimObject hierarchy to an FST scope tree
and records dispatches of statically owned SimObject events during simulation.

Unlike RTL simulators that toggle clock signals every cycle, gem5 is event-driven:
computation happens only when events fire.
Rather than synthesizing fake clock toggles, we trace **actual event dispatches** —
each event becomes a 1-bit pulse signal inside its hosting SimObject's FST scope.

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
    fst_trace_hier.test.cc   # GTest: hierarchy + event pulse round-trips
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
- Within each scope, create 1-bit wire per explicitly owned static event belonging to that SimObject
- Signal names come from `event->name()` after replacing spaces and dots with `_`

### 1.3 FST File Lifecycle

| gem5 Phase | Action |
|------------|--------|
| Constructor | Store params, register exit callback for `fstWriterClose()` |
| `init()` | Collect and sort SimObjects for hierarchy emission |
| `startup()` | Walk `Event::allEvents`, require explicit ownership for static events, open FST file, write hierarchy + signals, install dispatch hooks on all EventQueues, set `dumpActive` from `start_active` param |
| Simulation | Dispatch hook checks `dumpActive` flag, emits 1-bit pulses (1→0 pairs) to FST just before functional event processing; Python or ROI markers can toggle tracing |
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
    fstWriterEmitValueChange(self->fstCtx, it->second, "0");
}
```

The `"1"` → `"0"` pair at the same timestamp produces a clean pulse in waveform
viewers. FST preserves ordering within the same time step.

The hook must run after `setCurTick(event->when())` and after the squashed-event check,
but immediately before `event->process()`. That placement matches the intended meaning:
the waveform pulse indicates that gem5 is about to execute the functional code for this event.

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
- Test event pulse signals: create 1-bit wires, emit pulses at known times,
  read back with `fstReaderIterBlocks()`, verify values + timestamps.
- Test blackout regions: emit pulses, call `fstWriterEmitDumpActive(ctx, 0)`,
  emit more pulses, call `fstWriterEmitDumpActive(ctx, 1)`, read back and verify
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

**Status:** Future extension, not part of the current implementation scope.

**Goal:** Add `FstStatBinding` configuration; periodically sample selected stats
and emit value changes to the FST file.

### 2.1 Design

- User configures `FstStatBinding` objects: target SimObject + stat name
- In `startup()`, resolve each stat via `statistics::Group::resolveStat()`
- Create FST real-valued signals (`FST_VT_VCD_REAL`) alongside event signals
- Schedule periodic `sampleStats` event (configurable period, default 10000 ticks)
- Handler reads `ScalarInfo::result()`, emits value change only when value differs

### 2.2 Stage 2 Tests

**GTest** (`skip_lib=True`): Write FST real-valued signals, read back, verify.

**Python system test**: Reuse traffic-gen + memory config, add stat bindings,
verify stat samples in FST output.

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

## Python Configuration Interface (Stage 1)

```python
class FstTrace(SimObject):
    type = "FstTrace"
    trace_file = Param.String("trace.fst", "Output file name")
    compression = Param.String("lz4", "Compression: zlib, lz4, fastlz")
    timescale = Param.Int(-12, "FST timescale exponent (-12 = 1ps)")
    start_active = Param.Bool(True, "Start with dump enabled")
    use_work_item_roi = Param.Bool(False,
        "Toggle tracing from work item ROI markers when available")
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

## Files to Modify (Stage 1)

| File | Change |
|------|--------|
| `src/sim/sim_object.hh` | Add `static const vector<SimObject*> &getSimObjectList()` |
| `src/sim/eventq.hh` | Add `static vector<Event*> allEvents` in Event; add `dispatchHook` / `dispatchHookArg` raw pointers on EventQueue |
| `src/sim/eventq.cc` | Register/deregister in Event ctor/dtor; call `dispatchHook` in `serviceOne()` immediately before `process()` |
| `src/sim/eventq.hh` / `src/sim/eventq.cc` | Add side registry for static event ownership; erase entries on destruction |
| `src/sim/eventq.hh` | Change static event wrappers so they require owning `SimObject` as the first constructor argument |

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
