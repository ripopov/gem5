# Lesson 6: C++ Stats, Debug Flags, and Logging

This lesson covers gem5's three core observability systems: the statistics
framework, debug-flag-controlled tracing, and structured logging.

The goal is to make four ideas concrete:

1. The statistics framework (`statistics::Scalar`, `Vector`, `Distribution`)
   lets every component collect and report quantitative data.
2. Debug flags and `DPRINTF` provide selective, per-component trace output
   that can be toggled at runtime.
3. Logging functions (`panic`, `fatal`, `warn`, `inform`) give structured
   severity levels for error handling and diagnostics.
4. Stats are organized hierarchically through `statistics::Group`, mirroring
   the SimObject tree.

## Status

Placeholder. Code examples will be authored in a later change.

## Planned scope

1. Defining scalar, vector, and distribution statistics on a SimObject.
2. Registering stats with `statistics::Group` and the naming hierarchy.
3. Declaring a `debug::SimpleFlag` and using `DPRINTF` / `DPRINTFS`.
4. Using `panic()`, `fatal()`, `warn()`, and `inform()` correctly.
5. Dumping and resetting statistics programmatically.
6. Enabling debug flags from the command line and in tests.

## Key gem5 classes

### Statistics (`src/base/statistics.hh`, `src/base/stats/group.hh`)

- `statistics::Scalar`: single counter (e.g., cache hits).
- `statistics::Vector`: indexed array of counters.
- `statistics::Distribution`: histogram of sampled values.
- `statistics::Formula`: derived stat computed from other stats.
- `statistics::Group`: hierarchical container that mirrors the SimObject
  tree and provides `regStats()` / `resetStats()` hooks.

### Debug flags (`src/base/debug.hh`, `src/base/trace.hh`)

- `debug::SimpleFlag`: a named boolean flag toggled at runtime.
- `debug::CompoundFlag`: aggregates multiple simple flags.
- `DPRINTF(flag, fmt, ...)`: prints if `flag` is enabled; prepends
  tick and object name automatically.
- `DPRINTFS(flag, obj, fmt, ...)`: same but explicitly names the object.
- `DPRINTFN(fmt, ...)`: unconditional trace (no flag check).

### Logging (`src/base/logging.hh`)

- `panic(fmt, ...)`: bug in gem5 itself; aborts immediately.
- `fatal(fmt, ...)`: user configuration error; exits cleanly.
- `warn(fmt, ...)`: something suspicious but non-fatal.
- `inform(fmt, ...)`: informational message (always printed).

## Why this lesson matters for later lessons

Every component built in later lessons will use stats, DPRINTF, and
logging. Understanding these APIs early makes debugging and validation
straightforward from the start.
