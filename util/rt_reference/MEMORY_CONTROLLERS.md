# One versus two memory controllers

These historical measurements used 100 ns transport delays and quantum.
Current defaults are five CPU/device cycles (5 ns) per direction and a 5 ns
quantum; timing and speedup must be remeasured for that configuration.

The same 16-accelerator FP32 reduction ran with one and two independent
`SimpleMemory` controllers. Each retains **32 GiB/s nominal bandwidth**,
**100 ns latency (100 cycles at 1 GHz)** and queue 0 placement. Two controllers
interleave the unchanged 64 MiB RAM aperture on address bit 6: alternating
64-byte cache lines. They provide 64 GiB/s aggregate nominal bandwidth.
The host, firmware, input, tile size, DMA credits, clocks, crossbars,
transport delays and synchronization quantum are otherwise identical.

These are fixed-latency memory models, without DRAM row/bank timing.
Python combines the unchanged host ELF and input ELF into `boot.elf`;
`RiscvBareMetal` initializes both controllers through the physical proxy
before simulation. Input initialization consumes no simulated host cycles.
The loader retains the host entry point and symbol/debug sections.

All 16 partials are **262,144.0f**; the host sum is **4,194,304.0f** in every
configuration. Both controllers serve exactly 8 MiB of reduction input;
the traffic table below also counts descriptor reads and result writes.

| Controllers | Aggregate GiB/s | Batch cycles | Input B/cycle | Median q1 wall (s) | Median q17 wall (s) | PDES speedup |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 32 | 523,734 | 32.034 | 33.439877 | 5.377759 | 6.218× |
| 2 | 64 | 314,022 | 53.427 | 23.589597 | 4.894348 | 4.820× |

Two controllers improve modeled batch completion by
**1.668×** over one controller.
Batch cycles cover job submission through final terminal-status checks.
Input throughput is 16 MiB divided by this interval, in 1 GHz system cycles.
Simulation wall time covers return from instantiation through return from
`simulate()`, including boot, validation/export and the explicit batch dump.
Automatic exit statistics and cleanup are excluded; whole-process timings
and their separate medians are in the [raw JSON](MEMORY_CONTROLLERS.json).

| Controllers | Controller | DMA read bytes | DMA write bytes |
| --- | --- | ---: | ---: |
| 1 | `system.memory` | 16,777,600 | 64 |
| 2 | `system.memory` | 8,388,800 | 64 |
| 2 | `system.memory1` | 8,388,800 | 0 |

`SimpleMemory` rounds its bandwidth parameter to integer ticks per byte.
Each 32 GiB/s controller uses 29 ticks/byte, or a ceiling of 34.483 B/system
cycle; the pair's ceiling is 68.966 B/cycle. A separate **one-controller
64 GiB/s** control uses 15 ticks/byte (66.667 B/cycle), completes in
**314,530 cycles**, and is excluded from timing medians.
The ratio of that control's completion time to the two-controller time is
**1.002×**. Nominal aggregate bandwidth is equal,
but the quantized bandwidth limits and request arbitration differ.

For each controller count, one and seventeen queues have identical batch
and final ticks, every simulated statistic across both dumps, and all 33
byte-identical transaction traces. Each untraced pair also matches its traced
reference. Across controller counts, outputs, descriptors and initial images
match; model-time statistics and traces change as expected with the memory
topology. Host runtime/process-memory counters are excluded from comparisons.

| Controllers | Pair | q1 simulation wall (s) | q17 simulation wall (s) |
| --- | ---: | ---: | ---: |
| 1 | 0 | 33.439877 | 5.377759 |
| 1 | 1 | 33.462901 | 5.536192 |
| 1 | 2 | 33.331162 | 5.354776 |
| 2 | 0 | 24.117047 | 4.894348 |
| 2 | 1 | 23.187014 | 5.317019 |
| 2 | 2 | 23.589597 | 4.829813 |

Runs were sequential, alternating controller and queue order between pairs.
The harness checked for competing gem5 processes before, during and after
each run. Traces and the bandwidth control do not contribute to wall medians.
Host work, thermal state and OS scheduling can still affect wall samples.
Machine: **Intel(R) Core(TM) Ultra 7 265K**, `Z890-AORUS`;
affinity: `0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19`.
Platform: `Linux-7.0.0-38-generic-x86_64-with-glibc2.43`.

Reproduce from the repository root, with existing RISCV binary and firmware:

```sh
make -C util/rt_reference reduction
python3 util/rt_reference/compare_memory_controllers.py \
    --gem5 build/RISCV/gem5.opt --output m5out/memory-controllers \
    --runs 3
python3 util/rt_reference/report_memory_controllers.py \
    m5out/memory-controllers/results.json --output /tmp/MEMORY_CONTROLLERS.md
```

Use a fresh output directory. Reinspect with the comparison command plus
`--inspect-only`; executable, firmware and source hashes must match the
recorded artifacts. Exact commands, hashes, all samples, comparison evidence
and per-controller traffic are in the raw JSON. Base commit:
`36ebb2984cd19cbce3c996dd9440e6831cf04892`. Configuration and DMA details are in
[REDUCTION.md](REDUCTION.md); its validation notes describe the earlier O3
crash and the scope of previous sanitizer checks.
