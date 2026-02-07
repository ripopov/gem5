# Lesson 5: C++ ClockedObjects

This lesson introduces `ClockedObject`, the standard base class for
clock-aware SimObjects in gem5.

The goal is to make four ideas concrete:

1. `ClockedObject` combines `SimObject` lifecycle with `Clocked`
   cycle/tick helpers.
2. Clocked components schedule work on clock edges using
   `clockEdge()` / `nextCycle()`.
3. Runtime behavior should be expressed in cycles, then converted
   to ticks through domain-aware helpers.
4. `clockPeriodUpdated()` is the hook for adapting model state when
   domain frequency changes.

## Status

Placeholder. Code examples will be authored in a later change.

## Planned scope

1. Building a minimal `ClockedObject` with a pulse event.
2. Scheduling events with `clockEdge(Cycles n)` and `nextCycle()`.
3. Using `curCycle()`, `cyclesToTicks()`, and `ticksToCycles()` in object
   logic.
4. Overriding `clockPeriodUpdated()` to respond to clock changes.
5. Demonstrating domain inheritance and explicit `clk_domain` overrides.
6. Testing cycle-aligned behavior across multiple clock domains.

## Key gem5 classes

### `ClockedObject` (`src/sim/clocked_object.hh`)

- Base class for SimObjects that need clock-domain semantics.
- Provides conversion helpers and edge-aligned scheduling primitives.

### `Clocked` (`src/sim/clocked_object.hh`)

- Core cycle/tick conversion API used by `ClockedObject`.
- Maintains per-object alignment to domain clock edges.

### `ClockDomain` (`src/sim/clock_domain.hh`)

- Supplies period and voltage context to all registered clocked members.
- Propagates period updates to members and derived domains.

## Why this lesson matters for later lessons

Most timing-accurate gem5 components inherit `ClockedObject`. Learning this
class directly is the bridge between abstract clock-domain concepts and
practical model implementation.
