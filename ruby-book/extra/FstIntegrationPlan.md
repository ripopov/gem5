# Plan: Integrate libfst Waveform Tracing into gem5

## Context

gem5 has no built-in waveform dump capability.
Hardware designers are accustomed to viewing simulation results in waveform viewers (GTKWave, Surfer).
The libfst library is already vendored at `ext/libfst/` with full writer/reader support.
The goal is to create an `FstTrace` SimObject that automatically maps the SimObject hierarchy to an FST scope tree
and records event dispatches during simulation.

Unlike RTL simulators that toggle clock signals every cycle, gem5 is event-driven:
computation happens only when events fire.
Rather than synthesizing fake clock toggles, we trace **actual event dispatches** —
each event becomes a 1-bit pulse signal inside its hosting SimObject's FST scope.

The implementation is split into three stages, each independently testable.

---

## Core Mechanism: Event Self-Registration + Streaming FST Writes

Events in gem5 are predominantly constructed at elaboration time as member variables
of SimObjects (~182 static members across the codebase). By adding self-registration
to the `Event` constructor, we can discover all events before simulation starts,
create FST signals upfront, and stream value changes during simulation — no buffering needed.

### Event self-registration

Add a static `std::vector<Event*>` to the `Event` class.
In the constructor, push `this`; in the destructor, remove it.
At `startup()`, FstTrace walks this registry, extracts event names,
resolves them to SimObjects via longest-prefix matching, and creates FST signals.

### Event name → SimObject mapping

Event naming patterns in gem5:
- `EventFunctionWrapper::name()` → `"<simobject_path>.<event_name>.wrapped_function_event"`
- `MemberEventWrapper::name()` → `"<simobject_path>.wrapped_event"`
- Inner event classes → `"Event_<id>"` (no path; only `description()` is semantic)

**Parsing strategy:** At `startup()`, collect all SimObject names from `simObjectList`.
For each registered event, find the longest SimObject name that is a prefix of the event name.
The remainder after the prefix becomes the event's short signal name.
Events that don't match go into a top-level "unresolved" scope.

### Streaming FST writes

Since all signals are known at `startup()`, we open the FST file, write the hierarchy,
and emit value changes in real time via the dispatch callback.
No buffering, no deferred writes.

### Dynamic events

~116 events are created dynamically during simulation (retries, syscall callbacks, etc.).
These register in the global list but won't have FST signals since the hierarchy is already written.
V1 simply skips them; future versions could add a catch-all string signal.

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

When null, cost is a single pointer comparison — truly zero overhead.
No `std::function` (avoids heap allocation and indirect vtable call on the hot path).

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

## Stage 1: Hierarchy Dump + Event Tracing

**Goal:** Self-register events, walk SimObject hierarchy, create FST scope tree
with per-event signals, stream event dispatches during simulation.

### 1.1 Event Self-Registration

Modify `Event` class (`src/sim/eventq.hh`, `src/sim/eventq.cc`):
- Add `static std::vector<Event*> allEvents`
- Constructor: `allEvents.push_back(this)`
- Destructor: erase from `allEvents`

### 1.2 SimObject Hierarchy → FST Scopes

- Add `static const vector<SimObject*> &getSimObjectList()` to SimObject
- Sort by `name()` (full dotted path like `system.cpu0.icache`)
- Track `vector<string> currentScope`; compute push/pop per object:
  - `fstWriterSetScope(ctx, FST_ST_VCD_MODULE, component, NULL)` per new level
  - `fstWriterSetUpscope(ctx)` per level to pop
- Within each scope, create 1-bit wire per event belonging to that SimObject

### 1.3 FST File Lifecycle

| gem5 Phase | Action |
|------------|--------|
| Constructor | Store params, register exit callback for `fstWriterClose()` |
| `init()` | Collect SimObject names for prefix matching |
| `startup()` | Walk `Event::allEvents`, resolve to SimObjects, open FST file, write hierarchy + signals, install dispatch hook on main EventQueue, set `dumpActive` from `start_active` param |
| Simulation | Dispatch hook checks `dumpActive` flag, emits 1-bit pulses (1→0 pairs) to FST; Python script can call `setDumpActive()` between `m5.simulate()` calls for ROI control |
| Exit callback | Close FST file |

All `fstWriterCreateVar()` calls complete in `startup()` before any value changes.

### 1.4 Dispatch Hook

At `startup()`, install a static trampoline on the main `EventQueue`:

```cpp
// In startup():
mainEventQueue[0]->dispatchHook = &FstTrace::dispatchTrampoline;
mainEventQueue[0]->dispatchHookArg = this;

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

`eventHandleMap` is `std::unordered_map<const Event*, fstHandle>`, built once at
`startup()`. Can be upgraded to a faster flat hash map later if profiling warrants.

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

**Future C++ integration:**

The C++ `setDumpActive()` method can also be called from within SimObject code,
enabling programmatic ROI control from CPU models, workload markers, or
magic instructions (e.g., `m5_work_begin` / `m5_work_end` pseudo-ops).

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

## Python Configuration Interface (Final)

```python
class FstProbeBinding(SimObject):                                                # Stage 3
    type = "FstProbeBinding"
    target = Param.SimObject("SimObject owning the probe point")
    probe_name = Param.String("Name of the probe point")
    signal_name = Param.String("", "Override name in FST")
    bit_width = Param.Unsigned(64, "Bit width of the FST signal")
    probe_type = Param.String("uint64", "Type: uint64, bool, packet")

class FstStatBinding(SimObject):                                                 # Stage 2
    type = "FstStatBinding"
    target = Param.SimObject("SimObject owning the stat")
    stat_name = Param.String("Stat name relative to the SimObject")
    signal_name = Param.String("", "Override name in FST")

class FstTrace(SimObject):
    type = "FstTrace"
    trace_file = Param.String("trace.fst", "Output file name")
    compression = Param.String("lz4", "Compression: zlib, lz4, fastlz")
    timescale = Param.Int(-12, "FST timescale exponent (-12 = 1ps)")
    start_active = Param.Bool(True, "Start with dump enabled")                   # Stage 1
    probe_bindings = VectorParam.FstProbeBinding([], "Probe→signal bindings")    # Stage 3
    stat_bindings = VectorParam.FstStatBinding([], "Stat→signal bindings")       # Stage 2
    stat_sample_period = Param.Tick(10000, "Ticks between stat samples")         # Stage 2
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
| `src/sim/eventq.cc` | Register/deregister in Event ctor/dtor; call `dispatchHook` in `serviceOne()` |

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
| **Dynamic events without FST signals** | Skip in dispatch callback; ~116 dynamic events vs ~182 static |
| **Event name parsing** | Longest-prefix match against SimObject names; "unresolved" scope for misses |
| **Dispatch hook overhead** | Raw function pointer: single null check when disabled; O(1) hash lookup when enabled. Map can be upgraded to flat hash map if profiling shows need |
| **Event destructor cost** | Linear scan of `allEvents` to erase; acceptable for rare destruction |
| **Thread safety** | `allEvents` populated before simulation (single-threaded); dispatch on main queue |
