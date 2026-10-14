# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""Capture one raytrace smoke sample for each optimized sweep configuration."""

import argparse
import json
import os
import shutil
import subprocess
import time
from pathlib import Path

from experiment import (
    ROOT,
    machine_info,
    require_idle,
    simulation_processes,
)
from measure_host_render import inspect as inspect_host
from report_raytrace import (
    accelerator_metrics,
    digest,
    load,
    relative,
)
from validate_backend import (
    GOLDEN,
    compare,
    inspect,
)

QUANTUMS = (1, 5, 10, 20, 50, 100, 1000)


def utc():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def save(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def configurations():
    for cores in (8, 16):
        yield "Accelerators", cores, 1, 5
        for quantum in QUANTUMS:
            yield "Accelerators", cores, cores + 1, quantum
    yield "Host", 8, 1, 5


def directory_for(output, renderer, cores, queues, quantum):
    name = "host" if renderer == "Host" else f"accel{cores}"
    return output / name / f"{quantum}ns-q{queues}"


def command_for(binary, directory, renderer, cores, queues, quantum):
    command = [str(binary), f"--outdir={directory}"]
    if renderer == "Host":
        command += [
            "--debug-flags=Quiesce",
            "--debug-ignore=system.cpu*",
            "--debug-file=wfi.log",
        ]
    command += [
        str(ROOT / "configs/example/rt_accel.py"),
        "--backend=gem5",
        "--accelerator-cpu=o3",
        f"--accelerator-cores={cores}",
        f"--queues={queues}",
        "--render-mode=1",
        "--samples-per-pixel=1",
        "--max-secondary-depth=0",
        "--seed=1",
        f"--transport-delay={quantum}ns",
    ]
    if renderer == "Host":
        command.append("--host-render")
    return command


def run_once(command, directory):
    before = simulation_processes()
    require_idle(before)
    record = {
        "command": command,
        "started_utc": utc(),
        "idle_machine_checked": True,
        "simulations_before": before,
        "process_checks": 0,
    }
    save(directory / "started.json", record)
    started = time.perf_counter()
    with (directory / "run.log").open("w") as log:
        with subprocess.Popen(
            command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT
        ) as process:
            try:
                while process.poll() is None:
                    require_idle(simulation_processes(), process.pid)
                    record["process_checks"] += 1
                    try:
                        process.wait(timeout=1)
                    except subprocess.TimeoutExpired:
                        pass
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait()
            record["returncode"] = process.returncode
    record.update(
        wall_seconds=time.perf_counter() - started,
        finished_utc=utc(),
        simulations_after=simulation_processes(),
    )
    require_idle(record["simulations_after"])
    save(directory / "wall.json", record)
    return record


def collect_run(output, renderer, cores, queues, quantum):
    directory = directory_for(output, renderer, cores, queues, quantum)
    wall = load(directory / "wall.json")
    if wall["returncode"]:
        return {
            **wall,
            "status": "failed",
            "renderer": renderer,
            "accelerators": cores,
            "queues": queues,
            "quantum_ticks": quantum * 1000,
            "artifact_directory": relative(directory),
        }
    if renderer == "Host":
        metric = {**inspect_host(directory), **wall}
        metric["artifact_directory"] = relative(directory)
        metric["raw_artifact_sha256"] = {
            name: digest(directory / name)
            for name in (
                "stats.txt",
                "config.json",
                "parameters.json",
                "wall.json",
                "run.log",
                "wfi.log",
            )
        }
    else:
        validation = inspect(directory, queues, "o3", cores)
        validation.update(wall, repeat=0)
        save(directory / "validation.json", validation)
        metric = accelerator_metrics(validation, directory)
        metric.update(wall)
    assert metric["quantum_ticks"] == quantum * 1000
    assert metric["transport_delay"] == f"{quantum}ns"
    assert metric["memory_controllers"] == 4
    assert metric["rgba_sha256"] == GOLDEN
    parameters = load(directory / "parameters.json")
    assert parameters["trace"] is False
    assert parameters["queue_depth"] == 16
    assert parameters["dma_outstanding"] == 256
    metric.update(
        status="passed",
        samples=1,
        matched_simulated_counters=None,
        pdes_wall_speedup=None,
        full_simulated_cycles_per_wall_second=(
            metric["full_simulated_cycles"] / wall["wall_seconds"]
        ),
    )
    save(directory / "metrics.json", metric)
    return metric


def compare_pairs(result, output):
    for cores in (8, 16):
        serial = next(
            run
            for run in result["runs"]
            if run["renderer"] == "Accelerators"
            and run["accelerators"] == cores
            and run["queues"] == 1
        )
        if serial["status"] != "passed":
            continue
        for run in result["runs"]:
            if (
                run["renderer"] == "Accelerators"
                and run["accelerators"] == cores
                and run["status"] == "passed"
            ):
                run["pdes_wall_speedup"] = (
                    serial["wall_seconds"] / run["wall_seconds"]
                )
        parallel = next(
            run
            for run in result["runs"]
            if run["renderer"] == "Accelerators"
            and run["accelerators"] == cores
            and run["queues"] == cores + 1
            and run["quantum_ticks"] == 5000
        )
        if parallel["status"] != "passed":
            continue
        comparison = compare(
            ROOT / serial["artifact_directory"],
            ROOT / parallel["artifact_directory"],
            traces=False,
        )
        result["queue_pair_comparisons"].append(
            {"accelerators": cores, **comparison}
        )
        for run in (serial, parallel):
            run["matched_simulated_counters"] = comparison[
                "simulated_counters_compared"
            ]
    result["configurations"] = result["runs"]


def capture(output, binary):
    previous = load(ROOT / "util/rt_reference/RAYTRACE_RESULTS.json")
    sources = set(previous["source_sha256"]) | {
        "src/sim/eventq.cc",
        "src/sim/eventq.hh",
        "src/sim/global_event.cc",
        "src/sim/global_event.hh",
        "src/sim/quantum_barrier.hh",
        "src/sim/async_event_batch.hh",
        "src/arch/riscv/isa.cc",
        "src/arch/riscv/isa.hh",
        "util/rt_reference/measure_raytrace.py",
        "util/rt_reference/measure_host_render.py",
        "util/rt_reference/report_raytrace.py",
    }
    artifacts = output / "artifacts"
    artifacts.mkdir()
    shutil.copy2(binary, artifacts / "gem5.opt")
    firmware = {}
    for name in ("accelerator.elf", "host-gem5.elf", "host-render.elf"):
        path = ROOT / "util/rt_reference/out" / name
        firmware[name] = digest(path)
        shutil.copy2(path, artifacts / name)
    source_hashes = {}
    for name in sorted(sources):
        source_hashes[name] = digest(ROOT / name)
        destination = artifacts / "source" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, destination)
    return {
        "base_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "initial_git_status": subprocess.check_output(
            ["git", "status", "--short"], cwd=ROOT, text=True
        ),
        "gem5_sha256": digest(binary),
        "firmware_sha256": firmware,
        "source_sha256": source_hashes,
        "machine": {
            **machine_info(),
            "pid_namespace": os.readlink("/proc/self/ns/pid"),
        },
        "started_utc": utc(),
        "finished_utc": None,
        "samples_per_configuration": 1,
        "workload": {
            "width": 96,
            "height": 96,
            "mode": 1,
            "samples_per_pixel": 1,
            "max_secondary_depth": 0,
            "seed": 1,
        },
        "runs": [],
        "queue_pair_comparisons": [],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--gem5", type=Path, default=ROOT / "build/RISCV/gem5.opt"
    )
    parser.add_argument("--inspect-only", action="store_true")
    args = parser.parse_args()
    output, binary = args.output.resolve(), args.gem5.resolve()
    for name in ("GEM5_QUANTUM_SPINS",):
        assert name not in os.environ, f"Remove tuning override {name}"
    if args.inspect_only:
        result = load(output / "sweep.json")
        assert result["finished_utc"]
        result["runs"] = []
        result["queue_pair_comparisons"] = []
    else:
        require_idle(simulation_processes())
        output.mkdir(parents=True, exist_ok=False)
        result = capture(output, binary)
        save(output / "sweep.json", result)
    for index, (renderer, cores, queues, quantum) in enumerate(
        configurations(), 1
    ):
        directory = directory_for(output, renderer, cores, queues, quantum)
        if not args.inspect_only:
            directory.mkdir(parents=True)
            print(
                f"START {index}/17: {renderer}, {cores} accelerators, "
                f"{queues} queues, {quantum} ns",
                flush=True,
            )
            command = command_for(
                binary, directory, renderer, cores, queues, quantum
            )
            run_once(command, directory)
        run = collect_run(output, renderer, cores, queues, quantum)
        result["runs"].append(run)
        save(output / "sweep.json", result)
        print(
            f"{run['status'].upper()} {index}/17: "
            f"wall={run['wall_seconds']:.3f}s",
            flush=True,
        )
    for name, sha in result["source_sha256"].items():
        # Allow inspector fixes while retaining the measured source hashes.
        if not (args.inspect_only and name == relative(Path(__file__))):
            assert digest(ROOT / name) == sha, name
    if args.inspect_only:
        result["inspection_source_sha256"] = {
            name: digest(ROOT / name) for name in result["source_sha256"]
        }
    assert digest(binary) == result["gem5_sha256"]
    for name, sha in result["firmware_sha256"].items():
        assert digest(ROOT / "util/rt_reference/out" / name) == sha
    compare_pairs(result, output)
    if not args.inspect_only:
        result["finished_utc"] = utc()
    save(output / "sweep.json", result)
    print("COMPLETE: one attempt per configuration; no retries", flush=True)


if __name__ == "__main__":
    main()
