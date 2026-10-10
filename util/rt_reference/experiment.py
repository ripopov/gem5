# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
"""Process timing, statistics parsing and ELF layout for kernel experiments."""

import json
import os
import platform
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def machine_info():
    return {
        "hostname": platform.node(),
        "platform": platform.platform(),
        "lscpu": subprocess.check_output(["lscpu"], text=True),
        "cpu_affinity": sorted(os.sched_getaffinity(0)),
    }


def simulation_processes():
    output = subprocess.check_output(
        ["ps", "-eo", "pid=,stat=,comm="], text=True
    )
    return [
        {"pid": int(pid), "state": state, "command": command}
        for pid, state, command in (
            line.split(maxsplit=2) for line in output.splitlines()
        )
        if "gem5" in command
    ]


def require_idle(processes, own_pid=None):
    competing = [
        process
        for process in processes
        if process["pid"] != own_pid and process["state"][0] not in "TZ"
    ]
    assert (
        not competing
    ), f"Competing simulations invalidate timing: {competing}"


def measured_run(command, directory, require_idle_machine=False):
    before = simulation_processes() if require_idle_machine else None
    if before is not None:
        require_idle(before)
    started = time.perf_counter()
    checks = 0
    with (directory / "run.log").open("w") as log:
        with subprocess.Popen(
            command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT
        ) as process:
            try:
                while process.poll() is None:
                    if require_idle_machine:
                        require_idle(simulation_processes(), process.pid)
                        checks += 1
                    try:
                        process.wait(timeout=1)
                    except subprocess.TimeoutExpired:
                        pass
                assert process.returncode == 0, process.returncode
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait()
    elapsed = time.perf_counter() - started
    after = simulation_processes() if require_idle_machine else None
    if after is not None:
        require_idle(after)
    evidence = {
        "wall_seconds": elapsed,
        "command": command,
        "idle_machine_checked": require_idle_machine,
        "process_checks": checks,
        "simulations_before": before,
        "simulations_after": after,
    }
    (directory / "wall.json").write_text(json.dumps(evidence, indent=2) + "\n")
    return evidence


def stats_blocks(directory):
    """Keep explicit batch dumps separate from the automatic exit dump."""
    blocks = []
    result = {}
    for line in (directory / "stats.txt").read_text().splitlines():
        if "Begin Simulation Statistics" in line:
            result = {}
        elif "End Simulation Statistics" in line:
            blocks.append(result)
        fields = line.split()
        if len(fields) > 1 and not line.startswith("-"):
            try:
                result[fields[0]] = float(fields[1])
            except ValueError:
                pass
    assert blocks, f"No statistics in {directory}"
    return blocks


def stats(directory):
    # The first explicit dump is the first submitted batch. Historical
    # firmware produced only the automatic exit dump.
    return stats_blocks(directory)[0]


def symbols(elf):
    result = {}
    for line in subprocess.check_output(
        ["riscv64-unknown-elf-nm", "-n", str(elf)], text=True
    ).splitlines():
        fields = line.split()
        if len(fields) == 3:
            result[fields[2]] = int(fields[0], 16)
    return result


def compare_statistics(left, right):
    """Require every simulated statistic, in every dump, to agree."""
    left_blocks, right_blocks = stats_blocks(left), stats_blocks(right)
    assert len(left_blocks) == len(right_blocks)
    a = {
        f"dump{i}/{name}": value
        for i, block in enumerate(left_blocks)
        for name, value in block.items()
    }
    b = {
        f"dump{i}/{name}": value
        for i, block in enumerate(right_blocks)
        for name, value in block.items()
    }
    keys = set(a) | set(b)
    compared = {
        key
        for key in keys
        if not key.split("/", 1)[1].startswith(("host", "memUsage"))
    }
    mismatches = {
        key: (a.get(key), b.get(key))
        for key in compared
        if a.get(key) != b.get(key)
        and not (str(a.get(key)) == str(b.get(key)) == "nan")
    }
    assert not mismatches, (
        f"{len(mismatches)} simulated statistics differ: "
        f"{dict(list(sorted(mismatches.items()))[:20])}"
    )
    return {
        "simulated_counters_compared": len(compared),
        "excluded_host_counters": sorted(keys - compared),
    }
