# Lesson 4: Clock Domains, ClockedObjects, and Cycle/Tick Conversion

This lesson is a runnable C++ example that builds on Lessons 1-3 and focuses
on time at the *clock-domain* level.

The goal is to make four ideas concrete:

1. A `SrcClockDomain` defines an operating clock period (and DVFS points).
2. A `DerivedClockDomain` tracks a parent with an integer divider.
3. `Clocked`/`ClockedObject` helpers convert cleanly between cycles and ticks.
4. Changing a source-domain perf level propagates period updates to members and
   derived domains.

## Source map

- Lesson API:
  `src/tutorial/lesson04/clock_domains.hh`
- Lesson behavior:
  `src/tutorial/lesson04/clock_domains.cc`
- Unit test and domain setup:
  `src/tutorial/lesson04/clock_domains.test.cc`
- Build integration:
  `src/tutorial/SConscript`
- Sphinx wrapper page including this file:
  `docs/tutorial/lesson-04-cpp-clock-domains.md`

## Build and run

Build the lesson test binary:

```bash
scons build/NULL/tutorial/lesson04_clock_domains.test.debug
```

Run the binary:

```bash
./build/NULL/tutorial/lesson04_clock_domains.test.debug
```

List test cases:

```bash
./build/NULL/tutorial/lesson04_clock_domains.test.debug --gtest_list_tests
```

Run one test case:

```bash
./build/NULL/tutorial/lesson04_clock_domains.test.debug \
  --gtest_filter=ClockDomainsTest.DvfsChangesPeriodsAndPropagatesToDerivedClock
```

## Time, ticks, clocks, and periods

This section is the core vocabulary for timing in gem5.

### 1) Simulation time vs wall-clock time

gem5 tracks two very different notions of time:

- **Wall-clock time**: real elapsed host time (seconds on your workstation).
- **Simulation time**: modeled hardware time inside gem5.

When we say "tick 1000" in this lesson, we mean simulation time, not real
execution time on your host CPU.

### 2) Tick: gem5's global base time unit

A `Tick` is the smallest unit on gem5's global simulation timeline
(`src/base/types.hh`). Events are scheduled at absolute tick values on an
`EventQueue`.

In `src/sim/core.cc`, gem5's default global frequency is initialized to
`1e12` ticks/second, so by default:

- `1 tick = 1 ps`
- `1000 ticks = 1 ns`
- `1,000,000 ticks = 1 us`

Every clock domain, object, and event still references this *same* global tick
axis. Clock domains do not create separate time axes; they create separate
cycle interpretations over that one axis.

### 3) Clock and clock period

For a clocked component, two equivalent views exist:

- **Frequency** (cycles/second), e.g., 2 GHz.
- **Period** (time/cycle), e.g., 0.5 ns.

gem5 stores clock-domain timing primarily as **period in ticks**:

- `clockPeriod()` returns ticks per cycle.
- Smaller period means faster clock.

Given global ticks-per-second `Tps = sim_clock::Frequency`:

```text
frequency_hz = Tps / clock_period_ticks
clock_period_ticks = Tps / frequency_hz
```

With default `Tps = 1e12`:

- 2 GHz -> period = `1e12 / 2e9 = 500` ticks
- 1 GHz -> period = `1e12 / 1e9 = 1000` ticks

This is why Lesson 4 uses 500-tick and 1000-tick periods.

### 4) Cycles are domain-local, ticks are global

`Cycles` (`src/base/types.hh`) is a type-safe wrapper for cycle counts. A
cycle value only has meaning relative to a specific clock period/domain.

`Clocked` helpers (`src/sim/clocked_object.hh`) bridge the two spaces:

- `clockEdge(Cycles n)`: absolute tick for a future cycle boundary.
- `curCycle()`: cycle index aligned to current/next edge in this domain.
- `cyclesToTicks(c)`: convert cycle count to tick delta.
- `ticksToCycles(t)`: convert tick delta to cycles with ceil behavior.

Important nuance: `curCycle()` and `clockEdge()` align to the next edge if
`curTick()` is in the middle of a cycle. So these APIs intentionally snap
forward to edge-aligned semantics.

### 5) Multi-clock-domain intuition

Assume:

- source domain period = 500 ticks,
- derived domain period = 1000 ticks.

At global tick 750:

- source domain is between edges at 500 and 1000,
- derived domain is between edges at 0 and 1000.

Both domains return `clockEdge() == 1000`, but:

- source `nextCycle() == 1500`,
- derived `nextCycle() == 2000`.

Same global time, different local cycle progression.

### 6) What changes during DVFS

When `SrcClockDomain::perfLevel(...)` changes:

1. the source domain selects a new clock period,
2. voltage-domain performance level may adjust,
3. derived domains recompute their periods from parent period and divider,
4. registered clocked members receive `updateClockPeriod()` callbacks.

The key idea: simulation time remains one global tick timeline, while cycle
mapping per domain is updated.

## Why this lesson exists

Lesson 1 showed that gem5 uses a global `Tick` timeline and an event queue.
That is necessary, but not sufficient, for realistic timing models.

Real hardware is built from blocks running at different clocks:

- a CPU core clock,
- one or more cache clocks,
- an interconnect clock,
- memory-controller and I/O clocks.

gem5 models this with **clock domains**. A clocked component still schedules
events on the same global event queue, but it computes event times using its
own local cycle notion.

That is exactly what this lesson demonstrates with runnable C++ code:

- two clock domains (`source` and `derived`),
- two clocked probes bound to those domains,
- cycle/tick conversions and edge alignment at runtime,
- a DVFS switch that updates periods and voltage.

## Mental model

```text
Global event queue tick timeline:
  0 -------- 500 -------- 1000 -------- 1500 -------- 2000 ...

Source clock domain (period = 500 ticks):
  cycle 0     cycle 1      cycle 2       cycle 3

Derived clock domain (divider=2, period = 1000 ticks):
  cycle 0                 cycle 1                   cycle 2
```

Both domains share one global timeline, but their cycle boundaries differ.
`Clocked::clockEdge()` and `Clocked::curCycle()` expose this mapping.

## gem5 APIs used in this lesson

### 1) `VoltageDomain` (`src/sim/voltage_domain.hh`)

Defines voltage operating points shared by source clock domains on the same
rail.

This lesson configures:

- level 0: `1.0V`
- level 1: `0.9V`

### 2) `SrcClockDomain` (`src/sim/clock_domain.hh`)

A source domain owns one or more clock periods (DVFS points). In this lesson:

- perf level 0: `500` ticks (faster)
- perf level 1: `1000` ticks (slower)

`perfLevel(...)` changes both:

- the selected source period,
- the selected voltage-domain perf level (via sanitization).

### 3) `DerivedClockDomain` (`src/sim/clock_domain.hh`)

A derived domain period is:

`derived_period = parent_period * clk_divider`

This lesson uses `clk_divider = 2`, so:

- parent `500` -> derived `1000`
- parent `1000` -> derived `2000`

### 4) `Clocked` (`src/sim/clocked_object.hh`)

`Clocked` is the conversion utility behind `ClockedObject`.
Lesson class `ClockedProbe` directly inherits `Clocked` to expose:

- `clockPeriod()`
- `curCycle()`
- `clockEdge(Cycles n)`
- `nextCycle()`
- `ticksToCycles(Tick)`
- `cyclesToTicks(Cycles)`

It also overrides `clockPeriodUpdated()` to record period-change propagation.

## Lesson implementation walkthrough

### Header: `clock_domains.hh`

`ClockedProbe` combines:

- `Clocked`: domain-aware cycle/tick helpers,
- `EventManager`: queue scheduling support for pulse callbacks.

State tracked by each probe:

- trace lines (`traceLog`),
- pulse counter (`pulseCount`),
- clock-update counter (`clockUpdateCount`).

### Source: `clock_domains.cc`

Important methods:

1. `sample(tag)`
   logs current tick, period, cycle, current edge, and next edge.
2. `schedulePulse(cycles_from_now)`
   schedules a pulse event at `clockEdge(cycles_from_now)`.
3. `clockPeriodUpdated()`
   runs when the owning domain changes period and records that update.

Every trace line is normalized for testing, for example:

```text
tick=750 label=source_probe tag=after-dvfs period=1000 cycle=2
edge=1000 next=2000
```

## Test walkthrough

### Fixture setup

`ClockDomainsTest`:

- installs event queue 0 as current queue,
- drains any stale events,
- resets tick to 0 before/after each test.

### `ClockedHelpersConvertBetweenCyclesAndTicks`

Validates:

1. initial source/derived periods are `500` and `1000`,
2. helper conversions at tick 0 (`clockEdge`, `ticksToCycles`,
   `cyclesToTicks`),
3. mid-cycle behavior at tick `750`:
   - source aligns to cycle 2, edge 1000, next 1500,
   - derived aligns to cycle 1, edge 1000, next 2000.

### `DvfsChangesPeriodsAndPropagatesToDerivedClock`

Validates:

1. `source.perfLevel(1)` changes source period (`500 -> 1000`),
2. derived period follows (`1000 -> 2000`),
3. voltage domain follows (`1.0V -> 0.9V`),
4. both probes receive a `clockPeriodUpdated()` callback.

### `PulseSchedulingUsesEachProbeClockEdges`

Validates pulse events scheduled one cycle ahead:

- source pulse lands at tick `500`,
- derived pulse lands at tick `1000`.

This is the key operational behavior: one event queue, multiple domain-specific
cycle interpretations.

## Relationship to `ClockedObject`

Production components usually derive from `ClockedObject` (not bare
`Clocked`). `ClockedObject` inherits `Clocked` and adds:

- full `SimObject` lifecycle and params wiring,
- power-state model integration.

This lesson keeps the code minimal by using `Clocked` directly while exercising
the same clock conversion and update APIs that `ClockedObject` classes use.

## Why this matters for later lessons

With clock domains in place, you can now build realistic timing behavior:

- per-component latency in cycles,
- multi-frequency subsystems (core/cache/memory),
- DVFS experiments where performance levels change over time.

Later lessons will apply this to timing pipelines, integration hooks, and full
system composition.
