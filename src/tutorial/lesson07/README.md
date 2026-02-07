# Lesson 7: C++ Stats, Debug Flags, and Logging

This lesson builds an observability-focused `ClockedObject` and tests it
end-to-end with gem5's three core mechanisms:

1. `statistics::*` counters/formulas/distributions for quantitative data.
2. Debug flags with `DPRINTF`/`DPRINTFS` for selective trace output.
3. Structured logging (`inform`, `warn`, `fatal`, `panic`) for diagnostics.

## Source map

- Lesson params declaration:
  `src/tutorial/lesson07/TutorialStatsDebugDemo.py`
- Lesson API:
  `src/tutorial/lesson07/stats_debug_logging.hh`
- Lesson behavior:
  `src/tutorial/lesson07/stats_debug_logging.cc`
- Unit tests:
  `src/tutorial/lesson07/stats_debug_logging.test.cc`
- Build integration:
  `src/tutorial/SConscript`
- Sphinx wrapper page:
  `docs/tutorial/lesson-07-cpp-stats-debug-logging.md`

## Build and run

Build the lesson test binary:

```bash
scons build/NULL/tutorial/lesson07_stats_debug_logging.test.debug
```

Run the full test suite:

```bash
./build/NULL/tutorial/lesson07_stats_debug_logging.test.debug
```

List test cases:

```bash
./build/NULL/tutorial/lesson07_stats_debug_logging.test.debug \
  --gtest_list_tests
```

Run one test case:

```bash
./build/NULL/tutorial/lesson07_stats_debug_logging.test.debug \
  --gtest_filter=TutorialStatsDebugDemoTest.DebugFlagControlsDprintfVisibility
```

## Core vocabulary

### 1) Statistics group hierarchy

`statistics::Group` provides hierarchical structure for stats. In this lesson,
stats are split into two explicit child groups:

- `flow`: attempts/accepts/drops and acceptance ratio.
- `timing`: completions, latency stats, and clock update counters.

That structure mirrors how larger SimObjects organize observability by topic.

### 2) Scalar, vector, distribution, and formula stats

- `statistics::Scalar`: single running counter/value.
- `statistics::Vector`: indexed counter set (here: base vs. extended latency).
- `statistics::Distribution`: bucketed samples (here: latency in cycles).
- `statistics::Formula`: derived metric from other stats
  (accept ratio, average latency).

### 3) Debug flags and trace macros

- `DebugFlag("TutorialLesson07", ...)` defines a runtime switch.
- `DPRINTF(TutorialLesson07, ...)` emits only when the flag is enabled.
- `DPRINTFS(TutorialLesson07, this, ...)` emits with explicit object context.

This allows detailed traces without paying always-on logging cost/noise.

### 4) Logging severities

- `inform(...)`: expected informational lifecycle output.
- `warn(...)`: suspicious but non-fatal behavior.
- `fatal(...)`: user/configuration error, simulation cannot continue.
- `panic(...)`: simulator bug/invariant violation.

The demo uses `inform` and `warn` in normal flows, and constructor guardrails
use `fatal_if(...)` for invalid parameter values.

## Why this lesson exists

Without observability, model validation becomes guesswork. A practical model
should expose:

- lightweight counters and derived rates,
- focused traces that can be switched on only when needed,
- clear severity-driven logs for diagnostics and failures.

This lesson makes those patterns concrete in a minimal, runnable object.

## Implementation walkthrough

### Python params: `TutorialStatsDebugDemo.py`

The lesson declares parameters controlling issue behavior and completion
latency:

- issue count/start/stride in cycles,
- base latency plus configurable spread,
- optional drop policy (`drop_every`).

This keeps test scenarios deterministic and configurable.

### C++ object: `TutorialStatsDebugDemo`

The object issues tokens from `startup()` and either drops or accepts each
token:

- dropped tokens increment flow-drop stats and emit `warn(...)`,
- accepted tokens schedule completion events and sample latency stats.

`clockPeriodUpdated()` also records clock-update observability.

### Stats hierarchy and formulas

Two nested stat groups are defined:

- `FlowStats`:
  - `attempts`, `accepted`, `dropped`,
  - `acceptanceRatio = accepted / attempts`.
- `TimingStats`:
  - `completions`, `latencySamples`, `latencyCycleTotal`,
  - `latencyClassCounts` vector (`base`, `extended`),
  - `latencyCycles` distribution,
  - `averageLatencyCycles = latencyCycleTotal / latencySamples`.

The demo also exposes `resetObservabilityStats()` to show programmatic stat
reset paths.

### Debug traces and logging

The implementation emits trace lines behind `TutorialLesson07`:

- issue/accept/complete events via `DPRINTF`,
- object-explicit probes via `DPRINTFS`.

The test suite toggles trace global enable and flag state to verify that
output appears only when expected.

## Unit tests

`stats_debug_logging.test.cc` validates:

1. Stats hierarchy and values for accepted traffic.
2. Logging output (`inform`/`warn`) and stat reset behavior.
3. Debug flag gating for `DPRINTF`/`DPRINTFS`.

## Key takeaways

- Group stats by concern (`flow`, `timing`) using `statistics::Group`.
- Use formulas for derived metrics instead of manual recomputation.
- Keep debug traces behind explicit flags.
- Reserve `fatal`/`panic` for hard failures; prefer `inform`/`warn` for
  runtime diagnostics.
