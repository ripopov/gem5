This file explains how to build and run the tutorial tests.

Tutorial examples live in `src/tutorial/`. Each lesson adds one or more
Google Test binaries through `src/tutorial/SConscript`.

## Build tutorial tests

For the current Lesson 1 example:

```shell
scons build/NULL/tutorial/lesson01_event_timeline.test.opt
```

`NULL` is the fastest ISA target for unit-test style tutorial examples.

## Run tutorial tests

Run the full Lesson 1 tutorial test binary:

```shell
./build/NULL/tutorial/lesson01_event_timeline.test.opt
```

List test cases:

```shell
./build/NULL/tutorial/lesson01_event_timeline.test.opt --gtest_list_tests
```

Run a single test case:

```shell
./build/NULL/tutorial/lesson01_event_timeline.test.opt \
  --gtest_filter=EventTimelineTest.ProcessesCallbacksByTickThenPriority
```

## Where to find lesson code

- Lesson 1 source: `src/tutorial/lesson01/`
- Lesson docs (source-adjacent): `src/tutorial/`
- Rendered docs wrappers (Sphinx): `docs/tutorial/`

As additional lessons are implemented, their test targets will be added to
`src/tutorial/SConscript`.
