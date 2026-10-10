# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
"""Compare private accelerator L1s with the cacheless four-controller path."""

import argparse
import json
import statistics
from pathlib import Path

from experiment import (
    ROOT,
    compare_statistics,
    machine_info,
    measured_run,
    stats,
)
from validate_reduction import (
    CONFIG,
    artifact_files,
    compare,
    compare_outputs,
    digest,
    inspect,
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--gem5", type=Path, default=ROOT / "build/RISCV/gem5.opt"
    )
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--inspect-only", action="store_true")
    args = parser.parse_args()
    assert args.runs > 0
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    files = artifact_files(args.gem5) + [Path(__file__).resolve()]
    provenance = {str(p.relative_to(ROOT)): digest(p) for p in files}
    if args.inspect_only:
        saved = json.loads((args.output / "results.json").read_text())
        assert saved["artifact_sha256"] == provenance
    runs, comparisons = [], []

    def run(name, workload, queues, caches="l1", trace=False, extra=()):
        directory = args.output / name
        directory.mkdir(exist_ok=True)
        command = [
            str(args.gem5.resolve()),
            f"--outdir={directory}",
            str(CONFIG),
            f"--workload={workload}",
            f"--queues={queues}",
            f"--accelerator-caches={caches}",
            *extra,
        ]
        if trace:
            command.append("--trace")
        if not args.inspect_only:
            assert not (directory / "wall.json").exists()
            print("Running", name, flush=True)
            measured_run(command, directory, require_idle_machine=True)
        result = inspect(directory, workload, queues)
        result.update(name=name, caches=caches)
        values = stats(directory)
        if workload == "reduction":
            result["core_statistics"] = [
                {
                    suffix: values.get(f"system.accelerators{i}.{suffix}")
                    for suffix in (
                        "cpu.numCycles",
                        "cpu.lsq0.blockedByCache",
                        "cpu.rename.LQFullEvents",
                        "cpu.commitStats0.numLoadInsts",
                        "cpu.executeStats0.numLoadInsts",
                        "icache.overallHits::total",
                        "icache.overallMisses::total",
                        "dcache.overallHits::total",
                        "dcache.overallMisses::total",
                        "dcache.overallMissRate::total",
                        "control.cleanRequests",
                    )
                }
                for i in range(16)
            ]
        runs.append(result)
        print(name, result["batch_ticks"] / 1000, "cycles", flush=True)
        return result

    for caches in ("none", "l1"):
        for queues in (1, 2):
            run(
                f"dma-{caches}-q{queues}",
                "dma-test",
                queues,
                caches,
                True,
                ("--queue-depth=2", "--dma-outstanding=16"),
            )
        comparisons.append(
            compare(
                args.output / f"dma-{caches}-q1",
                args.output / f"dma-{caches}-q2",
                "dma-test",
                True,
            )
        )
    for queues in (1, 2):
        run(
            f"fault-q{queues}",
            "dma-fault",
            queues,
            trace=True,
            extra=("--queue-depth=2", "--dma-outstanding=16"),
        )
    comparisons.append(
        compare(
            args.output / "fault-q1",
            args.output / "fault-q2",
            "dma-fault",
            True,
        )
    )
    copy = run("copy", "copy", 1, trace=True)
    for direction in ("read", "write"):
        assert copy["wire"][0][f"{direction}_sustained_bytes_per_cycle"] >= 63
    for repeat in range(args.runs):
        modes = ("none", "l1") if repeat % 2 == 0 else ("l1", "none")
        queues = (1, 17) if repeat % 2 == 0 else (17, 1)
        for caches in modes:
            for queue in queues:
                run(f"{caches}-{repeat}-q{queue}", "reduction", queue, caches)
            comparisons.append(
                compare(
                    args.output / f"{caches}-{repeat}-q1",
                    args.output / f"{caches}-{repeat}-q17",
                    "reduction",
                    False,
                )
            )
        compare_outputs(
            args.output / f"none-{repeat}-q1",
            args.output / f"l1-{repeat}-q1",
            "reduction",
        )
        if repeat:
            for caches in modes:
                compare_statistics(
                    args.output / f"{caches}-0-q1",
                    args.output / f"{caches}-{repeat}-q1",
                )
    medians = {
        caches: {
            str(queue): statistics.median(
                r["simulation_wall"]["wall_seconds"]
                for r in runs
                if r["workload"] == "reduction"
                and r["caches"] == caches
                and r["queues"] == queue
            )
            for queue in (1, 17)
        }
        for caches in ("none", "l1")
    }
    cycles = {
        caches: next(
            r["batch_ticks"] / 1000
            for r in runs
            if r["workload"] == "reduction" and r["caches"] == caches
        )
        for caches in ("none", "l1")
    }
    result = dict(
        machine=machine_info(),
        artifact_sha256=provenance,
        runs=runs,
        comparisons=comparisons,
        batch_cycles=cycles,
        modeled_speedup=cycles["none"] / cycles["l1"],
        median_simulation_wall_seconds=medians,
        pdes_wall_speedup={c: m["1"] / m["17"] for c, m in medians.items()},
    )
    assert provenance == {str(p.relative_to(ROOT)): digest(p) for p in files}
    (args.output / "results.json").write_text(
        json.dumps(result, indent=2) + "\n"
    )
    print("PASS", cycles, "speedup", result["modeled_speedup"], flush=True)


if __name__ == "__main__":
    main()
