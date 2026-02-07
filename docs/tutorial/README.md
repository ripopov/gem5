# gem5 Simulator Construction Tutorial

This tutorial series teaches how to build a simulator in gem5 from working
source examples, starting with pure C++ internals and then moving to a hybrid
C++/Python workflow.

The end goal is a small but complete simulator stack:

- custom C++ modeling components,
- Python configuration and orchestration,
- reproducible runs, stats, and debugging workflows.

Only Lesson 1 has a complete code example right now. Lessons 2-10 are
scaffolded and their code examples will be added next.

## Learning path

Phase 1 (`pure C++`):

1. **Lesson 1**: Simulation kernel fundamentals (ticks, events, stats)
2. **Lesson 2**: SimObject anatomy and object lifecycle in C++
3. **Lesson 3**: Ports, packets, and memory request flow in C++
4. **Lesson 4**: Clocks, timing mode, and CPU-model integration hooks

Phase 2 (`C++ + Python`):

5. **Lesson 5**: Bridging C++ models into Python parameters/config
6. **Lesson 6**: Building full systems with Python configuration scripts
7. **Lesson 7**: Using the gem5 standard library composition model
8. **Lesson 8**: Workloads, resources, and checkpoints
9. **Lesson 9**: Debug flags, statistics pipelines, and probe points
10. **Lesson 10**: Capstone hybrid simulator assembly and validation

## Lesson index

1. [Lesson 1: C++ Simulation Kernel](lesson-01-cpp-simulation-kernel/README.md)
2. [Lesson 2: C++ SimObject Anatomy](lesson-02-cpp-simobject-anatomy/README.md)
3. [Lesson 3: C++ Memory and Ports](lesson-03-cpp-memory-and-ports/README.md)
4. [Lesson 4: C++ Timing and CPU Hooks](lesson-04-cpp-timing-and-cpu-hooks/README.md)
5. [Lesson 5: C++/Python Bridge](lesson-05-cpp-python-bridge/README.md)
6. [Lesson 6: Python Config and Run Loop](lesson-06-python-config-and-run/README.md)
7. [Lesson 7: gem5 Stdlib Composition](lesson-07-stdlib-composition/README.md)
8. [Lesson 8: Workloads and Checkpoints](lesson-08-workloads-and-checkpoints/README.md)
9. [Lesson 9: Debug, Stats, and Probes](lesson-09-debug-stats-and-probes/README.md)
10. [Lesson 10: Capstone Hybrid Simulator](lesson-10-capstone-hybrid-simulator/README.md)

## Source example map

1. Lesson 1 code: `src/tutorial/lesson01/` (implemented)
2. Lesson 2 code: `src/tutorial/lesson02/` (planned)
3. Lesson 3 code: `src/tutorial/lesson03/` (planned)
4. Lesson 4 code: `src/tutorial/lesson04/` (planned)
5. Lesson 5 code: `src/tutorial/lesson05/` (planned)
6. Lesson 6 code: `src/tutorial/lesson06/` (planned)
7. Lesson 7 code: `src/tutorial/lesson07/` (planned)
8. Lesson 8 code: `src/tutorial/lesson08/` (planned)
9. Lesson 9 code: `src/tutorial/lesson09/` (planned)
10. Lesson 10 code: `src/tutorial/lesson10/` (planned)

```{toctree}
:maxdepth: 2
:caption: Lessons

lesson-01-cpp-simulation-kernel/README
lesson-02-cpp-simobject-anatomy/README
lesson-03-cpp-memory-and-ports/README
lesson-04-cpp-timing-and-cpu-hooks/README
lesson-05-cpp-python-bridge/README
lesson-06-python-config-and-run/README
lesson-07-stdlib-composition/README
lesson-08-workloads-and-checkpoints/README
lesson-09-debug-stats-and-probes/README
lesson-10-capstone-hybrid-simulator/README
```
