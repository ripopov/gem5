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

1. Lesson 1: C++ Simulation Kernel (`src/tutorial/lesson01/README.md`)
2. Lesson 2: C++ SimObject Anatomy (`src/tutorial/lesson02/README.md`)
3. Lesson 3: C++ Memory and Ports (`src/tutorial/lesson03/README.md`)
4. Lesson 4: C++ Timing and CPU Hooks (`src/tutorial/lesson04/README.md`)
5. Lesson 5: C++/Python Bridge (`src/tutorial/lesson05/README.md`)
6. Lesson 6: Python Config and Run Loop (`src/tutorial/lesson06/README.md`)
7. Lesson 7: gem5 Stdlib Composition (`src/tutorial/lesson07/README.md`)
8. Lesson 8: Workloads and Checkpoints (`src/tutorial/lesson08/README.md`)
9. Lesson 9: Debug, Stats, and Probes (`src/tutorial/lesson09/README.md`)
10. Lesson 10: Capstone Hybrid Simulator (`src/tutorial/lesson10/README.md`)

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
