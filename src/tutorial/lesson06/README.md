# Lesson 6: C++ Hierarchical Modeling

This lesson builds on Lesson 5 by showing how to model a larger component as
multiple cooperating `ClockedObject` children, coordinated by a parent
`ClockedObject`.

The goal is to make four ideas concrete:

1. A parent object should own child objects with clear responsibilities.
2. Parent/child boundaries should be explicit function interfaces, not hidden
   shared state.
3. Each child can run in a different clock domain while still sharing one
   global tick timeline.
4. Parent-level policy (issue, queueing, dispatch) can be tested independently
   from child mechanism (latency and completion timing).

## Source map

- Lesson params declaration:
  `src/tutorial/lesson06/TutorialHierarchyDemo.py`
- Lesson API:
  `src/tutorial/lesson06/hierarchical_modeling.hh`
- Lesson behavior:
  `src/tutorial/lesson06/hierarchical_modeling.cc`
- Unit tests and domain setup:
  `src/tutorial/lesson06/hierarchical_modeling.test.cc`
- Build integration:
  `src/tutorial/SConscript`
- Sphinx wrapper page including this file:
  `docs/tutorial/lesson-06-cpp-hierarchical-modeling.md`

## Build and run

Build the lesson test binary:

```bash
scons build/NULL/tutorial/lesson06_hierarchical_modeling.test.debug
```

Run the binary:

```bash
./build/NULL/tutorial/lesson06_hierarchical_modeling.test.debug
```

List test cases:

```bash
./build/NULL/tutorial/lesson06_hierarchical_modeling.test.debug \
  --gtest_list_tests
```

Run one test case:

```bash
./build/NULL/tutorial/lesson06_hierarchical_modeling.test.debug \
  --gtest_filter=TutorialHierarchyDemoTest.ChildClockDomainsChangeCompletionTicks
```

## Core vocabulary

### 1) Hierarchy

A hierarchy is a parent model composed from child models. The parent owns
policy and coordination. Children own local mechanism.

In this lesson:

- Parent: `TutorialHierarchyDemo`
- Children: two `TutorialHierarchyStage` objects (`frontend`, `backend`)

### 2) Composition boundary

A composition boundary is the explicit interface between parent and child.

In this lesson the boundary is:

- parent -> child: `tryAccept(token, boundary)`
- child -> parent: completion callback with token id

This keeps communication intentional and testable.

### 3) Local clock semantics

Each child uses its own `clk_domain` for cycle/tick conversion.

- `frontend_clk_domain` controls frontend edge/latency mapping.
- `backend_clk_domain` controls backend edge/latency mapping.

All events still execute on the same global event queue and global tick axis.

### 4) Aggregation logic

Aggregation logic is the parent policy that coordinates children:

- issue new tokens,
- queue on backpressure,
- dispatch queued work when a child becomes idle,
- declare completion at the parent boundary.

## Why this lesson exists

A single monolithic model quickly mixes concerns:

- mechanism timing,
- buffering/backpressure,
- end-to-end policy.

Hierarchical modeling separates these concerns while preserving deterministic
simulator behavior.

## Lesson implementation walkthrough

### Python params: `TutorialHierarchyDemo.py`

This file declares two SimObjects:

- `TutorialHierarchyStage`: reusable child stage with `latency_cycles`.
- `TutorialHierarchyDemo`: parent aggregator with issue settings and per-child
  domain/latency parameters.

`frontend_clk_domain` and `backend_clk_domain` make child-domain selection an
explicit model decision.

### Child mechanism: `TutorialHierarchyStage`

Each stage:

- accepts one token at a time,
- schedules completion at `clockEdge() + cyclesToTicks(latency_cycles)`,
- logs edge/cycle/period data,
- notifies parent through a completion callback,
- counts `clockPeriodUpdated()` calls for DVFS propagation checks.

### Parent policy: `TutorialHierarchyDemo`

The parent:

- owns two child stage instances,
- issues a finite token stream from `startup()`,
- queues tokens when frontend/backend are busy,
- dispatches queues when children complete,
- records completion at the aggregate boundary.

This demonstrates policy/mechanism separation in a runnable model.

### Unit tests: `hierarchical_modeling.test.cc`

The test suite validates:

1. `ParentCoordinatesChildrenWithBackpressure`
   checks queueing and parent-level completion behavior.
2. `ChildClockDomainsChangeCompletionTicks`
   shows child-domain choices shift completion ticks for identical
   cycle delays.
3. `DvfsPropagatesToParentAndChildren`
   verifies source-domain DVFS updates parent and child clock-aware members.

## Key takeaways

- Model composition explicitly: parent policy + child mechanisms.
- Keep boundaries narrow and observable (`tryAccept` + completion callback).
- Treat per-child `clk_domain` as a first-order design choice.
- Use focused unit tests to validate both child timing and parent aggregation.
