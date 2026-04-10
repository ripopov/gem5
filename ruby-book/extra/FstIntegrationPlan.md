# Plan: Integrate libfst Waveform Tracing into gem5

## Context

gem5 has no built-in waveform dump capability.
Hardware designers are accustomed to viewing simulation results in waveform viewers (GTKWave, Surfer).
The libfst library is already vendored at `ext/libfst/` with full writer/reader support.
The goal is to create an `FstTrace` SimObject that automatically maps the SimObject hierarchy to an FST scope tree
and records value changes (clock ticks, stats, probes) during simulation.

The implementation is split into three stages, each independently testable.

---

## Directory Layout

```
src/sim/fst_trace/
    FstTrace.py          # Python SimObject definitions
    fst_trace.hh         # C++ header
    fst_trace.cc         # C++ implementation
    SConscript           # Build rules

tests/gem5/fst_trace/
    test_fst_trace.py    # Python test runner (invokes gem5 configs below)
    configs/
        fst_clocks.py    # Minimal system: Root + System + SrcClockDomain + a few ClockedObjects
        fst_stats.py     # Same system + stat sampling config
        fst_probes.py    # Same system + probe bindings
    ref/                 # Reference outputs (optional, for regression)
```

GTest-level unit tests use only the FST C API + reader to verify outputs;
they do **not** instantiate SimObjects (avoids Params codegen complexity).
Instead, the GTest tests verify the hierarchy-building algorithm and FST I/O
by calling FST writer functions directly and reading back with the FST reader.

Python-driven tests (`tests/gem5/fst_trace/`) instantiate a real gem5 system,
run a short simulation, and verify the FST output post-hoc.

---

## Stage 1: Hierarchy Dump + Clock Ticks

**Goal:** Walk `SimObject::simObjectList`, create matching FST scope tree,
and toggle clock signals for ClockedObjects.

### 1.1 SimObject Hierarchy → FST Scopes

- In `init()`, sort `simObjectList` by `name()` (full dotted path, e.g. `system.cpu0.icache`)
- Split each path on `.` into components
- Track a `vector<string> currentScope`; for each object compute push/pop:
  - `fstWriterSetScope(ctx, FST_ST_VCD_MODULE, component, NULL)` per new level
  - `fstWriterSetUpscope(ctx)` per level to pop
- Within each leaf scope, create signals as needed (clock wire for ClockedObjects)

### 1.2 Clock Tracing

- During the hierarchy walk, `dynamic_cast<ClockedObject*>` each SimObject
- For each hit, create a 1-bit FST wire (`FST_VT_VCD_WIRE`, width 1)
- Group by `ClockDomain*` (via `clockPeriod()`) to avoid duplicate toggle events
- In `startup()`, schedule one recurring event per unique domain at half-period intervals
- The handler toggles all signals in that domain and calls `fstWriterEmitValueChange`

### 1.3 FST File Lifecycle (Stage 1)

| gem5 Phase | Action |
|------------|--------|
| Constructor | `fstWriterCreate()`, set timescale (-12 = 1ps), compression (LZ4), version |
| `init()` | Walk simObjectList → build scope tree + create clock variables |
| `startup()` | Schedule clock toggle events; emit initial values at tick 0 |
| Simulation | Clock toggle event handlers emit value changes |
| Exit callback | `registerExitCallback()` → `fstWriterClose()` |

**Critical constraint:** All `fstWriterCreateVar()` must complete before any `fstWriterEmitValueChange()`.
The init→startup ordering guarantees this.

### 1.4 Stage 1 Tests

**GTest** (`src/sim/fst_trace/fst_trace_hier.test.cc`, `skip_lib=True`):
- Test the hierarchy push/pop algorithm in isolation:
  feed a sorted list of dotted names (e.g. `["a", "a.b", "a.b.c", "a.d", "e"]`)
  into the scope builder, write to a temp FST file, read back with `fstReaderIterateHier()`,
  and assert the scope tree matches expectations.
- Test clock signal creation: write a 1-bit wire, emit toggles at known times,
  read back with `fstReaderIterBlocks()` and verify values/timestamps.

**Python system test** (`tests/gem5/fst_trace/configs/fst_clocks.py`):
- Minimal config: traffic generator → crossbar → simple memory, with a `SrcClockDomain(clock='1GHz')`.
- Attach `FstTrace(trace_all_clocks=True)`.
- Run for a small number of ticks (e.g. 10000).
- Post-check: use `fst2vcd` or FST reader to verify scope hierarchy and clock toggles.

---

## Stage 2: Periodic Stat Sampling

**Goal:** Add `FstStatBinding` configuration; periodically sample selected stats
and emit value changes to the FST file.

### 2.1 Design

- User configures `FstStatBinding` objects: target SimObject + stat name
- In `init()`, resolve each stat via `statistics::Group::resolveStat()` on the target
- Create an FST real-valued signal (`FST_VT_VCD_REAL`) per stat
- In `startup()`, schedule a periodic `sampleStats` event (configurable period, default 10000 ticks)
- Handler reads `ScalarInfo::result()`, emits value change only if value differs from last sample

### 2.2 Stage 2 Tests

**GTest** (`src/sim/fst_trace/fst_trace_stats.test.cc`, `skip_lib=True`):
- Directly write FST real-valued signals at known times, read back, verify values.
- This tests the FST real-value encoding round-trip, not gem5 stats resolution.

**Python system test** (`tests/gem5/fst_trace/configs/fst_stats.py`):
- Reuse the Stage 1 traffic-gen + memory config.
- Add `stat_bindings` pointing at a traffic generator or memory stat.
- Run simulation, read FST output, verify stat signal has expected sample count
  and values are monotonically increasing for a counter stat.

---

## Stage 3: Probe Point Listeners

**Goal:** Add `FstProbeBinding` configuration; connect typed listeners to probe points
and emit value changes on each notification.

### 3.1 Design

- User configures `FstProbeBinding`: target SimObject, probe name, type, bit width
- In `regProbeListeners()`, create typed `ProbeListenerArgFunc<T>` for each binding
- Lambda callback calls `fstWriterEmitTimeChange()` + `fstWriterEmitValueChange()`
- Initial supported types:
  - `uint64` (PMU probes: `RetiredInsts`, `Cycles`) → 64-bit integer signal
  - `bool` (e.g. `Sleeping`) → 1-bit wire
  - `packet` (`probing::PacketInfo`) → multiple signals (addr, size, cmd)

### 3.2 Stage 3 Tests

**GTest** (`src/sim/fst_trace/fst_trace_probes.test.cc`, `skip_lib=True`):
- Test FST integer and multi-signal encoding round-trips.

**Python system test** (`tests/gem5/fst_trace/configs/fst_probes.py`):
- Reuse the traffic-gen + memory config.
- The crossbar/memory components already fire packet probes (`PktRequest`/`PktResponse`).
- Attach `FstProbeBinding(probe_type="packet")` to the crossbar.
- Run, read FST, verify probe-driven signals have non-zero event count at plausible timestamps.

---

## Python Configuration Interface (Final)

```python
class FstProbeBinding(SimObject):
    type = "FstProbeBinding"
    target = Param.SimObject("SimObject owning the probe point")
    probe_name = Param.String("Name of the probe point")
    signal_name = Param.String("", "Override name in FST (default: probe_name)")
    bit_width = Param.Unsigned(64, "Bit width of the FST signal")
    probe_type = Param.String("uint64", "Type: uint64, bool, packet")

class FstStatBinding(SimObject):
    type = "FstStatBinding"
    target = Param.SimObject("SimObject owning the stat")
    stat_name = Param.String("Stat name relative to the SimObject")
    signal_name = Param.String("", "Override name in FST")

class FstTrace(SimObject):
    type = "FstTrace"
    trace_file = Param.String("trace.fst", "Output file name")
    compression = Param.String("lz4", "Compression: zlib, lz4, fastlz")
    timescale = Param.Int(-12, "FST timescale exponent (-12 = 1ps)")
    trace_all_clocks = Param.Bool(False, "Trace clocks for all ClockedObjects")
    clock_objects = VectorParam.SimObject([], "Specific ClockedObjects to trace")
    probe_bindings = VectorParam.FstProbeBinding([], "Probe→signal bindings")       # Stage 3
    stat_bindings = VectorParam.FstStatBinding([], "Stat→signal bindings")           # Stage 2
    stat_sample_period = Param.Tick(10000, "Ticks between stat samples")             # Stage 2
```

---

## Build Integration

```python
# src/sim/fst_trace/SConscript
Import('*')

SimObject('FstTrace.py', sim_objects=['FstTrace', 'FstProbeBinding', 'FstStatBinding'])
Source('fst_trace.cc')
DebugFlag('FstTrace')

# Unit tests (skip_lib=True: no gem5 SimObject libraries needed, just FST API)
GTest('fst_trace_hier.test', 'fst_trace_hier.test.cc', skip_lib=True)   # Stage 1
GTest('fst_trace_stats.test', 'fst_trace_stats.test.cc', skip_lib=True) # Stage 2
GTest('fst_trace_probes.test', 'fst_trace_probes.test.cc', skip_lib=True) # Stage 3
```

Existing `ext/libfst/SConscript` already builds the library and adds include paths + link flags globally.

---

## Key Files to Reference

- `ext/libfst/fstapi.h` — FST writer/reader API
- `src/sim/sim_object.hh:153` — `simObjectList` static vector
- `src/sim/probe/probe.hh` — ProbePoint / ProbeListenerArgFunc templates
- `src/sim/clocked_object.hh` — ClockedObject, ClockDomain, clockPeriod()
- `src/sim/clock_domain.hh:96` — `ClockDomain::members` vector of `Clocked*`
- `src/base/stats/group.hh` — `statistics::Group::resolveStat()`
- `src/sim/sim_exit.hh:45` — `registerExitCallback()`
- `src/mem/probes/mem_trace.hh` — MemTraceProbe as reference pattern for probe listeners
- `src/base/fstapi.test.cc` — Existing GTest pattern for FST read/write round-trips

---

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| **Performance with trace_all_clocks** | Default `false`; require explicit opt-in |
| **Thread safety** | FST writer is not thread-safe; assert single-threaded for now, add mutex later |
| **Large hierarchies** | FST compressed hierarchy handles thousands of scopes — not a concern |
| **Stat resolution** | `resolveStat()` requires name relative to Group; fall back to searching `getStats()` by suffix |

---

## Open Questions

- Should the FST file path be relative to the gem5 output directory (`m5out/`) automatically?
- What is the right default stat sampling period — ticks or cycles?
- Should Port request/response activity be a first-class trace channel beyond generic probes?
