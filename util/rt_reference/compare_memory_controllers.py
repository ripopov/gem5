# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
"""Compare one/two RAM controllers on the same 16-accelerator reduction."""

import argparse
import json
import statistics
import subprocess
from pathlib import Path

from experiment import (
    ROOT,
    machine_info,
    measured_run,
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
    parser.add_argument(
        "--gem5", type=Path, default=ROOT / "build/RISCV/gem5.opt"
    )
    parser.add_argument(
        "--output", type=Path, default=ROOT / "m5out/memory-controllers"
    )
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--inspect-only", action="store_true")
    args = parser.parse_args()
    assert args.runs >= 1
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    paths = artifact_files(args.gem5) + [
        Path(__file__),
        ROOT / "util/rt_reference/report_memory_controllers.py",
    ]
    provenance = {str(p.resolve()): digest(p) for p in paths}
    if args.inspect_only:
        saved = json.loads((args.output / "results.json").read_text())
        assert saved["artifact_sha256"] == provenance
    runs, comparisons, workload_comparisons = [], [], []

    def run(controllers, queues, name, trace=False, bandwidth="32GiB/s"):
        directory = args.output / name
        directory.mkdir(exist_ok=True)
        command = [
            str(args.gem5.resolve()),
            f"--outdir={directory}",
            str(CONFIG),
            "--workload=reduction",
            f"--queues={queues}",
            f"--memory-controllers={controllers}",
            f"--memory-bandwidth={bandwidth}",
        ]
        if trace:
            command.append("--trace")
        if not args.inspect_only:
            assert not (
                directory / "wall.json"
            ).exists(), "Choose a fresh output directory"
            print("Running:", " ".join(command), flush=True)
            measured_run(command, directory, require_idle_machine=True)
        result = inspect(directory, "reduction", queues)
        result.update(run_name=name, bandwidth_per_controller=bandwidth)
        runs.append(result)
        return result

    for controllers in (1, 2):
        for queues in (1, 17):
            run(
                controllers,
                queues,
                f"correctness-mc{controllers}-q{queues}",
                True,
            )
        comparisons.append(
            compare(
                args.output / f"correctness-mc{controllers}-q1",
                args.output / f"correctness-mc{controllers}-q17",
                "reduction",
                True,
            )
        )
    workload_comparisons.append(
        compare_outputs(
            args.output / "correctness-mc1-q1",
            args.output / "correctness-mc2-q1",
            "reduction",
        )
    )
    for repeat in range(args.runs):
        for controllers in ((1, 2) if repeat % 2 == 0 else (2, 1)):
            for queues in ((1, 17) if repeat % 2 == 0 else (17, 1)):
                run(
                    controllers,
                    queues,
                    f"timing-{repeat}-mc{controllers}-q{queues}",
                )
            comparisons.append(
                compare(
                    args.output / f"timing-{repeat}-mc{controllers}-q1",
                    args.output / f"timing-{repeat}-mc{controllers}-q17",
                    "reduction",
                    False,
                )
            )
            comparisons.append(
                compare(
                    args.output / f"correctness-mc{controllers}-q1",
                    args.output / f"timing-{repeat}-mc{controllers}-q1",
                    "reduction",
                    False,
                )
            )
    # Separate controller count from aggregate nominal bandwidth. This
    # diagnostic is excluded from wall-time medians and PDES speedups.
    control = run(1, 1, "bandwidth-control-mc1-64GiB", bandwidth="64GiB/s")
    workload_comparisons.append(
        compare_outputs(
            args.output / "correctness-mc2-q1",
            args.output / control["run_name"],
            "reduction",
        )
    )
    summaries = {}
    for controllers in (1, 2):
        reference = next(
            r
            for r in runs
            if r["run_name"] == f"correctness-mc{controllers}-q1"
        )
        timing = [
            r
            for r in runs
            if r["run_name"].startswith("timing-")
            and r["memory_controllers"] == controllers
        ]
        simulation = {
            q: statistics.median(
                r["simulation_wall"]["wall_seconds"]
                for r in timing
                if r["queues"] == q
            )
            for q in (1, 17)
        }
        process = {
            q: statistics.median(
                r["process_wall"]["wall_seconds"]
                for r in timing
                if r["queues"] == q
            )
            for q in (1, 17)
        }
        summaries[controllers] = {
            "batch_cycles": reference["batch_ticks"] / 1000,
            "input_bytes_per_cycle": reference[
                "aggregate_input_bytes_per_batch_cycle"
            ],
            "median_simulation_wall_seconds": simulation,
            "median_process_wall_seconds": process,
            "pdes_speedup": simulation[1] / simulation[17],
            "process_speedup": process[1] / process[17],
        }
    assert provenance == {str(p.resolve()): digest(p) for p in paths}
    evidence = {
        "experiment": "memory-controllers",
        "runs_per_configuration": args.runs,
        "artifact_sha256": provenance,
        "git_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "machine": machine_info(),
        "runs": runs,
        "comparisons": comparisons,
        "workload_comparisons": workload_comparisons,
        "summaries": summaries,
        "two_controller_model_speedup": summaries[1]["batch_cycles"]
        / summaries[2]["batch_cycles"],
    }
    (args.output / "results.json").write_text(
        json.dumps(evidence, indent=2) + "\n"
    )
    print(
        "PASS: one/two controllers, reduction, queue equivalence; "
        f"modeled speedup={evidence['two_controller_model_speedup']:.3f}"
    )


if __name__ == "__main__":
    main()
