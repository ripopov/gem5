# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""Measure one eight-accelerator PDES sample per transport delay."""

import argparse
import json
import os
import signal
import subprocess
import time
from pathlib import Path

from experiment import (
    ROOT,
    machine_info,
    measured_run,
)
from report_raytrace import (
    digest,
    load,
    relative,
)
from validate_backend import (
    GOLDEN,
    check_transport,
    inspect,
)


def utc():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--quantums", type=int, nargs="+", default=(10, 20, 50, 100, 200, 1000)
    )
    parser.add_argument("--inspect-only", action="store_true")
    args = parser.parse_args()
    assert all(value > 0 for value in args.quantums)
    assert len(set(args.quantums)) == len(args.quantums)
    args.baseline = args.baseline.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    baseline = load(args.baseline / "validation.json")
    assert baseline["quantum"] == baseline["transport_delay"] == "5ns"
    assert baseline["accelerator_cores"] == 8
    assert baseline["accelerator_cpu"] == "o3"
    serial = next(run for run in baseline["runs"] if run["queues"] == 1)
    assert serial["repeat"] == 0 and serial["idle_machine_checked"]
    for name, sha in baseline["source_sha256"].items():
        assert digest(ROOT / name) == sha, name
    gem5 = ROOT / "build/RISCV/gem5.opt"
    assert digest(gem5) == baseline["gem5_sha256"]
    for name, sha in baseline["firmware_sha256"].items():
        assert digest(ROOT / "util/rt_reference/out" / name) == sha, name
    evidence_path = args.output / "sweep.json"
    if args.inspect_only:
        evidence = load(evidence_path)
        assert evidence["finished_utc"]
        assert evidence["quantums_ns"] == args.quantums
        old_runs = evidence["runs"]
    else:
        assert not evidence_path.exists(), "Choose a fresh output directory"
        runner = args.output / "measure_transport.executed.py"
        runner.write_bytes(Path(__file__).read_bytes())
        evidence = {
            "started_utc": utc(),
            "finished_utc": None,
            "git_commit": subprocess.check_output(
                ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
            ).strip(),
            "machine": {
                **machine_info(),
                "pid_namespace": os.readlink("/proc/self/ns/pid"),
            },
            "samples_per_configuration": 1,
            "accelerators": 8,
            "queues": 9,
            "quantums_ns": args.quantums,
            "baseline_directory": relative(args.baseline),
            "baseline_validation_sha256": digest(
                args.baseline / "validation.json"
            ),
            "speedup_baseline_transport_delay": "5ns",
            "speedup_baseline_wall_seconds": serial["wall_seconds"],
            "gem5_sha256": baseline["gem5_sha256"],
            "firmware_sha256": baseline["firmware_sha256"],
            "source_sha256": baseline["source_sha256"],
            "runner_sha256": digest(runner),
            "runner_artifact": relative(runner),
            "runs": [],
        }
        old_runs = []
    evidence["runs"] = []
    for index, quantum in enumerate(args.quantums):
        directory = args.output / f"{quantum}ns/q9-0"
        directory.mkdir(parents=True, exist_ok=True)
        command = [
            str(gem5),
            f"--outdir={directory}",
            str(ROOT / "configs/example/rt_accel.py"),
            "--backend=gem5",
            "--accelerator-cpu=o3",
            "--accelerator-cores=8",
            "--queues=9",
            "--render-mode=1",
            "--samples-per-pixel=1",
            "--max-secondary-depth=0",
            "--seed=1",
            f"--transport-delay={quantum}ns",
        ]
        if args.inspect_only:
            started = old_runs[index]["started_utc"]
            finished = old_runs[index]["finished_utc"]
            failed = old_runs[index].get("status") == "failed"
        else:
            started = utc()
            evidence["current_quantum_ns"] = quantum
            evidence_path.write_text(json.dumps(evidence, indent=2) + "\n")
            print(
                f"Running {index + 1}/{len(args.quantums)}: {quantum} ns",
                flush=True,
            )
            attempt_started = time.perf_counter()
            try:
                measured_run(command, directory, require_idle_machine=True)
                failed = False
            except AssertionError as error:
                # A failed process has no valid rendering wall-time sample.
                # Preserve its exit code and continue without retrying it.
                code = error.args[0] if error.args else None
                if not isinstance(code, int) or code == 0:
                    raise
                failed = True
                (directory / "failure.json").write_text(
                    json.dumps(
                        {
                            "returncode": code,
                            "signal": (
                                signal.Signals(-code).name
                                if code < 0
                                else None
                            ),
                            "failed_attempt_wall_seconds": (
                                time.perf_counter() - attempt_started
                            ),
                        },
                        indent=2,
                    )
                    + "\n"
                )
            finished = utc()
        if failed:
            run = load(directory / "failure.json")
            config = load(directory / "config.json")
            assert check_transport(config, f"{quantum}ns") == quantum * 1000
            assert load(directory / "parameters.json")["transport_delay"] == (
                f"{quantum}ns"
            )
            run.update(
                status="failed",
                quantum_ticks=quantum * 1000,
                transport_delay=f"{quantum}ns",
                accelerator_cores=8,
                queues=9,
                repeat=0,
                started_utc=started,
                finished_utc=finished,
                command=command,
                artifact_directory=relative(directory),
                wall_seconds=None,
                speedup_vs_5ns_serial=None,
            )
            evidence["runs"].append(run)
            print(
                f"FAILED: {quantum} ns, returncode={run['returncode']}",
                flush=True,
            )
            if not args.inspect_only:
                evidence_path.write_text(json.dumps(evidence, indent=2) + "\n")
            continue
        run = inspect(directory, 9, "o3", 8)
        parameters = load(directory / "parameters.json")
        expected = load(args.baseline / "q9-0/parameters.json")
        expected["transport_delay"] = f"{quantum}ns"
        assert parameters == expected
        assert run["quantum_ticks"] == 1000 * quantum
        assert run["sha256"] == GOLDEN
        for name in (
            "scene.bin",
            "job.bin",
            "image.rgba",
            "image.bmp",
            "parameters.bin",
            "core_count.bin",
            *(f"job{i}.bin" for i in range(8)),
        ):
            assert (directory / name).read_bytes() == (
                args.baseline / "q9-0" / name
            ).read_bytes(), name
        wall = load(directory / "wall.json")
        assert wall["command"] == command and wall["idle_machine_checked"]
        assert wall["process_checks"] > 0
        run.update(wall)
        run.update(
            status="complete",
            returncode=0,
            repeat=0,
            started_utc=started,
            finished_utc=finished,
            artifact_directory=relative(directory),
            speedup_vs_5ns_serial=serial["wall_seconds"]
            / wall["wall_seconds"],
        )
        evidence["runs"].append(run)
        print(
            f"PASS: {quantum} ns, {run['wall_seconds']:.3f} s, "
            f"{run['speedup_vs_5ns_serial']:.3f}x versus 5 ns serial",
            flush=True,
        )
        if not args.inspect_only:
            evidence_path.write_text(json.dumps(evidence, indent=2) + "\n")
    evidence.pop("current_quantum_ns", None)
    if not args.inspect_only:
        evidence["finished_utc"] = utc()
    evidence["inspection_runner_sha256"] = digest(Path(__file__))
    evidence["successful_samples"] = sum(
        run.get("status", "complete") == "complete" for run in evidence["runs"]
    )
    evidence["failed_attempts"] = (
        len(args.quantums) - evidence["successful_samples"]
    )
    evidence_path.write_text(json.dumps(evidence, indent=2) + "\n")
    print(
        f"Checked {len(args.quantums)} attempts, one per quantum: "
        f"{evidence['successful_samples']} successful, "
        f"{evidence['failed_attempts']} failed",
        flush=True,
    )


if __name__ == "__main__":
    main()
