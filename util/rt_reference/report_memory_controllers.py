# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
"""Render saved one/two-controller reduction measurements."""

import argparse
import json
import os
import re
from pathlib import Path


def report(results, source, output):
    raw_link = os.path.relpath(source.resolve(), output.resolve().parent)
    runs = {r["run_name"]: r for r in results["runs"]}
    summaries = results["summaries"]
    rows, traffic, samples = [], [], []
    for controllers in (1, 2):
        s = summaries[str(controllers)]
        wall = s["median_simulation_wall_seconds"]
        rows.append(
            f"| {controllers} | {controllers * 32} | "
            f"{s['batch_cycles']:,.0f} | {s['input_bytes_per_cycle']:.3f} | "
            f"{wall['1']:.6f} | {wall['17']:.6f} | {s['pdes_speedup']:.3f}× |"
        )
        reference = runs[f"correctness-mc{controllers}-q1"]
        for memory in reference["controller_statistics"]:
            traffic.append(
                f"| {controllers} | `{memory['path']}` | "
                f"{memory['dma_read_bytes']:,.0f} | "
                f"{memory['dma_write_bytes']:,.0f} |"
            )
        for repeat in range(results["runs_per_configuration"]):
            a = runs[f"timing-{repeat}-mc{controllers}-q1"]
            b = runs[f"timing-{repeat}-mc{controllers}-q17"]
            samples.append(
                f"| {controllers} | {repeat} | "
                f"{a['simulation_wall']['wall_seconds']:.6f} | "
                f"{b['simulation_wall']['wall_seconds']:.6f} |"
            )
    control = runs["bandwidth-control-mc1-64GiB"]
    control_cycles = control["batch_ticks"] / 1000
    split_cycles = summaries["2"]["batch_cycles"]
    cpu = re.search(r"Model name:\s+(.+)", results["machine"]["lscpu"])[1]
    comparisons = [c for c in results["comparisons"] if "trace_sha256" in c]
    assert len(comparisons) == 2
    header = (
        "| Controllers | Aggregate GiB/s | Batch cycles | Input B/cycle | "
        "Median q1 wall (s) | Median q17 wall (s) | PDES speedup |"
    )
    return f"""# One versus two memory controllers

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

{header}
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
{chr(10).join(rows)}

Two controllers improve modeled batch completion by
**{results['two_controller_model_speedup']:.3f}×** over one controller.
Batch cycles cover job submission through final terminal-status checks.
Input throughput is 16 MiB divided by this interval, in 1 GHz system cycles.
Simulation wall time covers return from instantiation through return from
`simulate()`, including boot, validation/export and the explicit batch dump.
Automatic exit statistics and cleanup are excluded; whole-process timings
and their separate medians are in the [raw JSON]({raw_link}).

| Controllers | Controller | DMA read bytes | DMA write bytes |
| --- | --- | ---: | ---: |
{chr(10).join(traffic)}

`SimpleMemory` rounds its bandwidth parameter to integer ticks per byte.
Each 32 GiB/s controller uses 29 ticks/byte, or a ceiling of 34.483 B/system
cycle; the pair's ceiling is 68.966 B/cycle. A separate **one-controller
64 GiB/s** control uses 15 ticks/byte (66.667 B/cycle), completes in
**{control_cycles:,.0f} cycles**, and is excluded from timing medians.
The ratio of that control's completion time to the two-controller time is
**{control_cycles / split_cycles:.3f}×**. Nominal aggregate bandwidth is equal,
but the quantized bandwidth limits and request arbitration differ.

For each controller count, one and seventeen queues have identical batch
and final ticks, every simulated statistic across both dumps, and all 33
byte-identical transaction traces. Each untraced pair also matches its traced
reference. Across controller counts, outputs, descriptors and initial images
match; model-time statistics and traces change as expected with the memory
topology. Host runtime/process-memory counters are excluded from comparisons.

| Controllers | Pair | q1 simulation wall (s) | q17 simulation wall (s) |
| --- | ---: | ---: | ---: |
{chr(10).join(samples)}

Runs were sequential, alternating controller and queue order between pairs.
The harness checked for competing gem5 processes before, during and after
each run. Traces and the bandwidth control do not contribute to wall medians.
Host work, thermal state and OS scheduling can still affect wall samples.
Machine: **{cpu}**, `{results['machine']['hostname']}`;
affinity: `{','.join(map(str, results['machine']['cpu_affinity']))}`.
Platform: `{results['machine']['platform']}`.

Reproduce from the repository root, with existing RISCV binary and firmware:

```sh
make -C util/rt_reference reduction
python3 util/rt_reference/compare_memory_controllers.py \\
    --gem5 build/RISCV/gem5.opt --output m5out/memory-controllers \\
    --runs {results['runs_per_configuration']}
python3 util/rt_reference/report_memory_controllers.py \\
    m5out/memory-controllers/results.json --output /tmp/MEMORY_CONTROLLERS.md
```

Use a fresh output directory. Reinspect with the comparison command plus
`--inspect-only`; executable, firmware and source hashes must match the
recorded artifacts. Exact commands, hashes, all samples, comparison evidence
and per-controller traffic are in the raw JSON. Base commit:
`{results['git_commit']}`. Configuration and DMA details are in
[REDUCTION.md](REDUCTION.md); its validation notes describe the earlier O3
crash and the scope of previous sanitizer checks.
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(
        report(json.loads(args.results.read_text()), args.results, args.output)
    )


if __name__ == "__main__":
    main()
