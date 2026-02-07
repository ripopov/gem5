This file explains how to build and run the tutorial tests.

Tutorial examples live in `src/tutorial/`. Each lesson adds one or more
Google Test binaries through `src/tutorial/SConscript`.

## Build tutorial tests

Current implemented lesson targets:

```shell
scons build/NULL/tutorial/lesson01_event_timeline.test.debug
scons build/NULL/tutorial/lesson02_simobject_lifecycle.test.debug
scons build/NULL/tutorial/lesson03_memory_ports.test.debug
scons build/NULL/tutorial/lesson04_clock_domains.test.debug
scons build/NULL/tutorial/lesson05_clocked_objects.test.debug
scons build/NULL/tutorial/lesson06_hierarchical_modeling.test.debug
```

`NULL` is the fastest ISA target for unit-test style tutorial examples.

## Run tutorial tests

Run a full tutorial test binary:

```shell
./build/NULL/tutorial/lesson01_event_timeline.test.debug
./build/NULL/tutorial/lesson02_simobject_lifecycle.test.debug
./build/NULL/tutorial/lesson03_memory_ports.test.debug
./build/NULL/tutorial/lesson04_clock_domains.test.debug
./build/NULL/tutorial/lesson05_clocked_objects.test.debug
./build/NULL/tutorial/lesson06_hierarchical_modeling.test.debug
```

List test cases:

```shell
./build/NULL/tutorial/lesson02_simobject_lifecycle.test.debug --gtest_list_tests
```

Run a single test case:

```shell
./build/NULL/tutorial/lesson02_simobject_lifecycle.test.debug \
  --gtest_filter=TutorialLifecycleDemoTest.DrainResumeRestoresDeferredEvent
```

## Where to find lesson code

- Lesson 1 source: `src/tutorial/lesson01/` (C++ Simulation Kernel)
- Lesson 2 source: `src/tutorial/lesson02/` (C++ SimObject Anatomy)
- Lesson 3 source: `src/tutorial/lesson03/` (C++ Memory and Ports)
- Lesson 4 source: `src/tutorial/lesson04/` (C++ Clock Domains)
- Lesson 5 source: `src/tutorial/lesson05/` (C++ ClockedObjects)
- Lesson 6 source: `src/tutorial/lesson06/` (C++ Hierarchical Modeling)
- Lesson 7 source: `src/tutorial/lesson07/` (C++ Stats, Debug Flags, and Logging)
- Lesson 8 source: `src/tutorial/lesson08/` (C++ Timing and CPU Hooks)
- Lesson 9 source: `src/tutorial/lesson09/` (C++ Serialization and Checkpointing)
- Lesson 10 source: `src/tutorial/lesson10/` (C++/Python Bridge)
- Lesson 11 source: `src/tutorial/lesson11/` (Python Config and Run Loop)
- Lesson 12 source: `src/tutorial/lesson12/` (gem5 Stdlib Composition)
- Lesson 13 source: `src/tutorial/lesson13/` (Workloads and Checkpoints)
- Lesson 14 source: `src/tutorial/lesson14/` (Capstone + Probes/Observability)
- Lesson docs (source-adjacent): `src/tutorial/`
- Rendered docs wrappers (Sphinx): `docs/tutorial/`

As additional lessons are implemented, their test targets are added to
`src/tutorial/SConscript`.
