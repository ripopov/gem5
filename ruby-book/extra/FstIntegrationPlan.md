# Plan: Integrate libfst Waveform Tracing into gem5

## Context

gem5 has no built-in waveform dump capability. Hardware designers are accustomed to viewing simulation results in waveform viewers (GTKWave, Surfer). The libfst library is already vendored at `ext/libfst/` with full writer/reader support. The goal is to create an `FstTrace` SimObject that:

1. Automatically maps the SimObject hierarchy to FST scope hierarchy
2. Traces clock toggles for ClockedObjects
3. Listens to probe points (packet probes, PMU counters, etc.) and records value changes
4. Periodically samples selected stats

The result is an `.fst` file viewable in GTKWave/Surfer, giving a familiar waveform-based view of gem5 simulation state.

## Files to Create

All new files go in `src/sim/fst_trace/`:

| File | Purpose |
|------|---------|
| `FstTrace.py` | Python SimObject definitions (FstTrace, FstProbeBinding, FstStatBinding) |
| `fst_trace.hh` | C++ header |
| `fst_trace.cc` | C++ implementation |
| `SConscript` | Build rules: SimObject, Source, DebugFlag |

## Key Design Decisions

### SimObject Hierarchy → FST Scopes

- In `init()`, iterate `SimObject::simObjectList` (all SimObjects in creation order)
- Sort by `name()` (full dotted path like `system.cpu0.icache`) for deterministic hierarchical ordering
- Track current scope path as a `vector<string>`; compare with each object's path components to compute push/pop operations:
  - `fstWriterSetScope(ctx, FST_ST_VCD_MODULE, component, NULL)` for each new level
  - `fstWriterSetUpscope(ctx)` for each level to pop
- Within each scope, create FST variables for clock/stats/probes as configured

### Clock Tracing

- During hierarchy walk in `init()`, `dynamic_cast<ClockedObject*>` each SimObject
- For each matched object, create a 1-bit FST wire signal (`FST_VT_VCD_WIRE`)
- Group objects by `ClockDomain` to avoid duplicate events
- In `startup()`, schedule a toggle event per unique clock domain at half-period intervals
- The event handler toggles the clock value and emits `fstWriterEmitValueChange` for all signals in that domain

### Probe Point Listeners

- User configures `FstProbeBinding` objects specifying: target SimObject, probe name, type (`uint64`/`bool`/`packet`), bit width
- In `regProbeListeners()`, create typed `ProbeListenerArgFunc<T>` for each binding
- The lambda callback calls `fstWriterEmitTimeChange` + `fstWriterEmitValueChange` with the probe value
- Support three initial types:
  - `uint64` (PMU probes like `RetiredInsts`, `Cycles`) → 64-bit integer signal
  - `bool` (e.g., `Sleeping`) → 1-bit wire
  - `packet` (`probing::PacketInfo`) → multiple signals (addr, size, cmd)

### Periodic Stat Sampling

- User configures `FstStatBinding` objects specifying: target SimObject, stat name
- In `init()`, resolve each stat via `statistics::Group::resolveStat()` on the target
- Create FST real-valued signals (`FST_VT_VCD_REAL`)
- Schedule a periodic `sampleStats` event (configurable period, default 10000 ticks)
- Only emit value changes when the value actually changed (delta compression)

### FST File Lifecycle

| gem5 Phase | FstTrace Action |
|------------|----------------|
| Constructor | Open FST file, set timescale (-12 = 1ps), compression (LZ4), version |
| `init()` | Walk simObjectList, build scope tree, create all FST variables |
| `regProbeListeners()` | Connect typed probe listeners to target probe points |
| `startup()` | Schedule clock toggle + stat sample events, emit initial values at tick 0 |
| Simulation | Event handlers + probe callbacks emit value changes |
| Exit callback | `registerExitCallback()` → `fstWriterClose()` to finalize file |

**Critical constraint:** All `fstWriterCreateVar()` calls must happen before any `fstWriterEmitValueChange()`. The init→startup ordering guarantees this.

## Python Configuration Interface

```python
class FstTrace(SimObject):
    trace_file = Param.String("trace.fst", "Output file name")
    compression = Param.String("lz4", "Compression: zlib, lz4, fastlz")
    timescale = Param.Int(-12, "FST timescale exponent (-12 = 1ps)")
    trace_all_clocks = Param.Bool(False, "Trace clocks for all ClockedObjects")
    clock_objects = VectorParam.SimObject([], "Specific ClockedObjects to trace")
    probe_bindings = VectorParam.FstProbeBinding([], "Probe→signal bindings")
    stat_bindings = VectorParam.FstStatBinding([], "Stat→signal bindings")
    stat_sample_period = Param.Tick(10000, "Ticks between stat samples")
```

Example usage:
```python
system.fst_trace = FstTrace(
    trace_all_clocks=True,
    probe_bindings=[
        FstProbeBinding(target=system.cpu, probe_name="RetiredInsts",
                        probe_type="uint64"),
    ],
    stat_bindings=[
        FstStatBinding(target=system.cpu, stat_name="ipc"),
    ],
)
```

## Build Integration

```python
# src/sim/fst_trace/SConscript
Import('*')
SimObject('FstTrace.py', sim_objects=['FstTrace', 'FstProbeBinding', 'FstStatBinding'])
Source('fst_trace.cc')
DebugFlag('FstTrace')
```

The existing `ext/libfst/SConscript` already builds the library and adds include paths + link flags globally.

## Key Files to Reference

- `ext/libfst/fstapi.h` — FST writer API
- `src/sim/sim_object.hh:153` — `simObjectList` static vector
- `src/sim/probe/probe.hh` — ProbePoint/ProbeListenerArgFunc templates
- `src/sim/clocked_object.hh` — ClockedObject, ClockDomain, clockPeriod()
- `src/base/stats/group.hh` — statistics::Group::resolveStat()
- `src/sim/sim_exit.hh:45` — `registerExitCallback()`
- `src/mem/probes/mem_trace.hh` — MemTraceProbe as reference pattern

## Verification

1. **Build**: `scons build/RISCV/gem5.opt -j$(nproc)` compiles without errors
2. **Smoke test**: Run a simple SE-mode config with `FstTrace(trace_all_clocks=True)`, verify `.fst` file is created and opens in GTKWave/Surfer with correct hierarchy
3. **Clock signals**: Verify clock toggles at expected period in waveform viewer
4. **Probe signals**: Attach a `uint64` probe to `RetiredInsts`, verify value changes appear
5. **Stat signals**: Configure stat sampling, verify periodic value updates in waveform
6. **Unit test**: Add `fst_trace.test.cc` with GTest to verify hierarchy building algorithm produces correct scope push/pop sequences

## Risks and Mitigations

- **Performance with trace_all_clocks**: Many clock domains = many events. Default to `false`, require explicit opt-in.
- **Thread safety**: FST writer is not thread-safe. Assert single-threaded simulation for now; add mutex later if needed.
- **Large hierarchies**: FST compressed hierarchy handles thousands of scopes efficiently — not a concern.

## Open Questions

- Should we support tracing Port activity (request/response) as a first-class feature beyond generic probes?
- Should the FST file path be relative to the gem5 output directory (`m5out/`) automatically?
- What is the right default stat sampling period — should it be in ticks or cycles?
- Should we add an `fst_trace` debug flag that logs every value change to the gem5 trace output for debugging the tracer itself?
