# Lesson 1: C++ Simulation Kernel Fundamentals

This lesson is implemented as a real C++ example in the source tree. The
markdown explains how to build and inspect that code.

## Example files

- `src/tutorial/lesson01/event_timeline.hh`
- `src/tutorial/lesson01/event_timeline.cc`
- `src/tutorial/lesson01/event_timeline.test.cc`
- Build integration: `src/tutorial/SConscript`

## What this example demonstrates

1. Scheduling events on a gem5 `EventQueue`.
2. Tick-based ordering (`tick=3` runs before `tick=7`).
3. Priority ordering when multiple events share the same tick.
4. Event self-rescheduling to create a callback chain.

## Build and run

Build only the tutorial test binary:

```bash
scons build/NULL/tutorial/lesson01_event_timeline.test.opt
```

Run it:

```bash
./build/NULL/tutorial/lesson01_event_timeline.test.opt
```

Optional: run only this test case:

```bash
./build/NULL/tutorial/lesson01_event_timeline.test.opt \
  --gtest_filter=EventTimelineTest.ProcessesCallbacksByTickThenPriority
```

## How to read the code

1. Open `src/tutorial/lesson01/event_timeline.hh`.
: This file defines `EventTimeline`, a tiny event-driven component used for the
lesson.
2. Open `src/tutorial/lesson01/event_timeline.cc`.
: This file shows the event scheduling logic and detailed comments around
callback ordering.
3. Open `src/tutorial/lesson01/event_timeline.test.cc`.
: This file executes the component and asserts the exact event trace and final
tick.

## Expected behavior

The test validates this exact callback sequence:

1. `tick=3 label=bootstrap`
2. `tick=7 label=high-priority-phase`
3. `tick=7 label=default-priority-phase`
4. `tick=7 label=low-priority-pulse`
5. `tick=9 label=low-priority-pulse`
6. `tick=11 label=low-priority-pulse`

This is the concrete baseline for later lessons that introduce SimObjects and
Python configuration.
