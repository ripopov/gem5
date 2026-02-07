# Lesson 5: C++ ClockedObjects

This lesson builds directly on Lesson 4 by moving from a helper class
(`Clocked`) to a full gem5 `ClockedObject`.

The goal is to make four ideas concrete:

1. `ClockedObject` combines `SimObject` lifecycle hooks with clock-domain
   timing helpers.
2. Runtime behavior should be described in cycles and converted to ticks for
   event scheduling.
3. `clockPeriodUpdated()` is the model hook used when DVFS changes domain
   period.
4. Choosing a `clk_domain` changes event timing even on one global event
   timeline.

## Source map

- Lesson params declaration:
  `src/tutorial/lesson05/TutorialClockedObjectDemo.py`
- Lesson API:
  `src/tutorial/lesson05/clocked_objects.hh`
- Lesson behavior:
  `src/tutorial/lesson05/clocked_objects.cc`
- Unit tests and domain setup:
  `src/tutorial/lesson05/clocked_objects.test.cc`
- Build integration:
  `src/tutorial/SConscript`
- Sphinx wrapper page including this file:
  `docs/tutorial/lesson-05-cpp-clocked-objects.md`

## Build and run

Build the lesson test binary:

```bash
scons build/NULL/tutorial/lesson05_clocked_objects.test.debug
```

Run the binary:

```bash
./build/NULL/tutorial/lesson05_clocked_objects.test.debug
```

List test cases:

```bash
./build/NULL/tutorial/lesson05_clocked_objects.test.debug --gtest_list_tests
```

Run one test case:

```bash
./build/NULL/tutorial/lesson05_clocked_objects.test.debug \
  --gtest_filter=TutorialClockedObjectDemoTest.DvfsUpdatesMembersAndChangesScheduling
```

## Time, ticks, clocks, and periods

This section is the core vocabulary for `ClockedObject` work.

### 1) Tick and cycle are different units

A `Tick` is global simulation time. A `Cycles` value is local to one clock
period. You can only interpret cycles after choosing a clock domain.

### 2) `ClockedObject` runs on one global timeline, not its own timeline

Every `ClockedObject` still schedules events on the same event queue and the
same global tick axis. A clock domain only changes how an object maps cycles
onto that global axis.

### 3) Edge-aligned helpers intentionally snap to a clock edge

`clockEdge()`, `curCycle()`, and `nextCycle()` are edge-aligned APIs.
If `curTick()` is in the middle of a cycle, they snap forward to the next
valid edge for that domain.

### 4) Convert with helpers, not ad hoc arithmetic

Use `cyclesToTicks()` and `ticksToCycles()` for conversions.
This keeps timing code tied to the active `clk_domain` period.

### 5) `clockPeriodUpdated()` is where DVFS-sensitive state adapts

When a domain period changes, gem5 calls `updateClockPeriod()` on members,
which then calls `clockPeriodUpdated()`. Override this hook to refresh model
state or bookkeeping that depends on period.

### 6) Clock-domain inheritance vs explicit override

`ClockedObject` defines `clk_domain = Parent.clk_domain` in Python, so child
objects inherit the parent domain unless explicitly overridden.

In this lesson test, we set `clk_domain` explicitly in C++ params to show the
same idea without a full Python system hierarchy.

## Why this lesson exists

Lesson 4 showed how domains and cycle/tick conversion work. Real gem5 models,
however, are usually `ClockedObject` subclasses that also need lifecycle hooks,
parameter plumbing, and event scheduling.

This lesson is the bridge from clock-domain concepts to practical component
implementation.

## Lesson implementation walkthrough

### Python params: `TutorialClockedObjectDemo.py`

`TutorialClockedObjectDemo` inherits `ClockedObject` and adds lesson-specific
parameters:

- `startup_cycles`: delay from `startup()` to first pulse,
- `pulse_count`: number of pulse callbacks,
- `pulse_stride_cycles`: cycles between pulses.

Because this is a SimObject declaration, SCons generates the C++ params struct
`TutorialClockedObjectDemoParams` used by the lesson test.

### C++ class: `clocked_objects.hh` / `clocked_objects.cc`

`TutorialClockedObjectDemo` inherits `ClockedObject` and demonstrates:

- lifecycle hooks (`init()` and `startup()`),
- cycle-based scheduling through `schedulePulse(Cycles)`,
- conversion helpers (`cyclesToTicks()`, `ticksToCycles()`),
- edge helpers (`curCycle()`, `clockEdge()`, `nextCycle()`),
- DVFS hook (`clockPeriodUpdated()`).

Two trace logs are recorded:

- `lifecycleTrace()`: construction/init/startup markers,
- `timingTrace()`: period/cycle/edge snapshots for pulse and sample events.

Scheduling uses this pattern:

```text
target_tick = clockEdge() + cyclesToTicks(cycles_from_now)
```

That keeps the model cycle-oriented while still scheduling on global ticks.

### Unit tests: `clocked_objects.test.cc`

The test suite creates:

- one source domain (`500` -> `1000` ticks under DVFS),
- one derived domain (divider `2`).

Test coverage is split by behavior:

1. `ClockHelpersExposeCycleTickConversions`
   validates edge helpers and conversion helpers.
2. `StartupSchedulesCycleAlignedPulses`
   validates lifecycle-driven pulse scheduling and repeat stride.
3. `ExplicitClockDomainsChangePulseTicks`
   shows one-cycle pulses land at different ticks for source vs derived domain.
4. `DvfsUpdatesMembersAndChangesScheduling`
   validates period propagation, `clockPeriodUpdated()` callbacks, and
   post-DVFS scheduling behavior.

## Key takeaways

- Prefer cycle-centric model logic; convert via `ClockedObject` helpers.
- Use edge-aligned APIs (`clockEdge`, `curCycle`, `nextCycle`) to avoid
  half-cycle ambiguity.
- Treat `clockPeriodUpdated()` as mandatory for state derived from period.
- `clk_domain` choice is a first-order timing decision even with one global
  event queue.
