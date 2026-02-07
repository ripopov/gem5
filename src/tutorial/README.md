# gem5 Simulator Construction Tutorial

This tutorial series teaches how to build a simulator in gem5 from working
source examples, starting with pure C++ internals and then moving to a hybrid
C++/Python workflow.

The end goal is a small but complete simulator stack:

- custom C++ modeling components,
- Python configuration and orchestration,
- reproducible runs, stats, and debugging workflows.

Lessons 1-5 have complete code examples right now. Lesson 6 currently has a
problem statement scaffold, and Lessons 7-14 are planned.

## Learning path

Phase 1 (`pure C++`):

1. **Lesson 1**: Simulation kernel fundamentals (ticks, events, scheduling)
2. **Lesson 2**: SimObject anatomy and object lifecycle in C++
3. **Lesson 3**: Ports, packets, and memory request flow in C++
4. **Lesson 4**: Clock domains and cycle/tick conversion
5. **Lesson 5**: ClockedObjects: clock-aware SimObjects and scheduling
6. **Lesson 6**: Hierarchical modeling with composed ClockedObjects
7. **Lesson 7**: Statistics, debug flags, and logging in C++
8. **Lesson 8**: Timing mode, latency modeling, and CPU-model integration hooks
9. **Lesson 9**: Serialization and checkpointing in C++

Phase 2 (`C++ + Python`):

10. **Lesson 10**: Bridging C++ models into Python parameters/config
11. **Lesson 11**: Building full systems with Python configuration scripts
12. **Lesson 12**: Using the gem5 standard library composition model
13. **Lesson 13**: Workloads, resources, and checkpoints
14. **Lesson 14**: Capstone hybrid simulator assembly, probes, and validation

## Lesson index

1. Lesson 1: C++ Simulation Kernel (`src/tutorial/lesson01/README.md`)
2. Lesson 2: C++ SimObject Anatomy (`src/tutorial/lesson02/README.md`)
3. Lesson 3: C++ Memory and Ports (`src/tutorial/lesson03/README.md`)
4. Lesson 4: C++ Clock Domains (`src/tutorial/lesson04/README.md`)
5. Lesson 5: C++ ClockedObjects (`src/tutorial/lesson05/README.md`)
6. Lesson 6: C++ Hierarchical Modeling (`src/tutorial/lesson06/README.md`)
7. Lesson 7: C++ Stats, Debug Flags, and Logging (`src/tutorial/lesson07/README.md`)
8. Lesson 8: C++ Timing and CPU Hooks (`src/tutorial/lesson08/README.md`)
9. Lesson 9: C++ Serialization and Checkpointing (`src/tutorial/lesson09/README.md`)
10. Lesson 10: C++/Python Bridge (`src/tutorial/lesson10/README.md`)
11. Lesson 11: Python Config and Run Loop (`src/tutorial/lesson11/README.md`)
12. Lesson 12: gem5 Stdlib Composition (`src/tutorial/lesson12/README.md`)
13. Lesson 13: Workloads and Checkpoints (`src/tutorial/lesson13/README.md`)
14. Lesson 14: Capstone + Probes/Observability (`src/tutorial/lesson14/README.md`)

## Source example map

1. Lesson 1 code: `src/tutorial/lesson01/` (implemented)
2. Lesson 2 code: `src/tutorial/lesson02/` (implemented)
3. Lesson 3 code: `src/tutorial/lesson03/` (implemented)
4. Lesson 4 code: `src/tutorial/lesson04/` (implemented)
5. Lesson 5 code: `src/tutorial/lesson05/` (implemented)
6. Lesson 6 code: `src/tutorial/lesson06/` (problem statement scaffold)
7. Lesson 7 code: `src/tutorial/lesson07/` (planned)
8. Lesson 8 code: `src/tutorial/lesson08/` (planned)
9. Lesson 9 code: `src/tutorial/lesson09/` (planned)
10. Lesson 10 code: `src/tutorial/lesson10/` (planned)
11. Lesson 11 code: `src/tutorial/lesson11/` (planned)
12. Lesson 12 code: `src/tutorial/lesson12/` (planned)
13. Lesson 13 code: `src/tutorial/lesson13/` (planned)
14. Lesson 14 code: `src/tutorial/lesson14/` (planned)
