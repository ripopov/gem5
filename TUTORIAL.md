This file explains how to build and run the tutorial tests.

Tutorial examples live in `src/tutorial/`. Each lesson adds one or more
Google Test binaries through `src/tutorial/SConscript`.

## Build tutorial tests

Current implemented lesson targets:

```shell
scons build/NULL/tutorial/lesson01_event_timeline.test.debug
scons build/NULL/tutorial/lesson02_simobject_lifecycle.test.debug
```

`NULL` is the fastest ISA target for unit-test style tutorial examples.

## Run tutorial tests

Run a full tutorial test binary:

```shell
./build/NULL/tutorial/lesson01_event_timeline.test.debug
./build/NULL/tutorial/lesson02_simobject_lifecycle.test.debug
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

- Lesson 1 source: `src/tutorial/lesson01/`
- Lesson 2 source: `src/tutorial/lesson02/`
- Lesson docs (source-adjacent): `src/tutorial/`
- Rendered docs wrappers (Sphinx): `docs/tutorial/`

As additional lessons are implemented, their test targets are added to
`src/tutorial/SConscript`.
