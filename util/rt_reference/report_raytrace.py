# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""Recheck ray-tracing measurements and generate JSON and Markdown results."""

import argparse
import copy
import hashlib
import json
import textwrap
from pathlib import Path

from experiment import ROOT
from validate_backend import (
    GOLDEN,
    check_transport,
    compare,
)


def load(path):
    return json.loads(path.read_text())


def digest(path):
    sha = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1 << 20):
            sha.update(chunk)
    return sha.hexdigest()


def relative(path):
    return str(path.resolve().relative_to(ROOT))


def cache_summary(cores, name):
    counters = {
        counter: sum(
            core["cache_statistics"][f"{name}.{counter}::total"]
            for core in cores
        )
        for counter in (
            "overallHits",
            "overallMisses",
            "overallMshrHits",
            "overallMshrMisses",
        )
    }
    accesses = counters["overallHits"] + counters["overallMisses"]
    return {
        "hits": counters["overallHits"],
        "misses": counters["overallMisses"],
        "hit_percent": 100 * counters["overallHits"] / accesses,
        "mshr_hits": counters["overallMshrHits"],
        "mshr_misses": counters["overallMshrMisses"],
        "mshr_miss_percent": 100 * counters["overallMshrMisses"] / accesses,
    }


def accelerator_metrics(run, directory):
    ipc = run["accelerator_ipc"]
    cores = ipc["per_core"]
    dma = {
        counter: sum(core["dma"][counter] for core in cores)
        for counter in (
            "bytesCopied",
            "busyTicks",
            "readRequests",
            "writeRequests",
            "cleanRequests",
            "readRetries",
            "writeRetries",
        )
    }
    return {
        "renderer": "Accelerators",
        "accelerators": len(cores),
        "active_render_cpus": len(cores),
        "queues": run["queues"],
        "memory_controllers": run["memory_controllers"],
        "quantum_ticks": run["quantum_ticks"],
        "transport_delay": run["transport_delay"],
        "full_simulated_cycles": run["simulation_clock_periods"],
        "batch_simulated_cycles": run["elapsed_clock_periods"],
        "batch_retired_instructions": ipc["retired_instructions"],
        "batch_counted_cpu_cycles": ipc["counted_cycles"],
        "weighted_core_ipc": ipc["cycle_weighted_core_ipc"],
        "cluster_ipc": ipc["combined_cluster_ipc"],
        "batch_megapixels_per_simulated_second": (
            run.get("width", 96)
            * run.get("height", 96)
            * 1000
            / run["elapsed_clock_periods"]
        ),
        "counted_cpu_occupancy_percent": (
            100
            * ipc["counted_cycles"]
            / (len(cores) * run["elapsed_clock_periods"])
        ),
        "dma_batch_duty_percent": (
            100 * dma["busyTicks"] / (len(cores) * run["simTicks"])
        ),
        "dma_busy_per_counted_cpu_cycle_percent": (
            100 * dma["busyTicks"] / (1000 * ipc["counted_cycles"])
        ),
        "dma_copied_bytes": int(dma["bytesCopied"]),
        "ram_read_bytes": sum(
            controller["read_bytes"]
            for controller in run["controller_statistics"]
        ),
        "ram_write_bytes": sum(
            controller["write_bytes"]
            for controller in run["controller_statistics"]
        ),
        "cache": {
            name: cache_summary(cores, name) for name in ("icache", "dcache")
        },
        "dma": dma,
        "per_core": cores,
        "controller_statistics": run["controller_statistics"],
        "stats_region_start_tick": run["stats_region_start_tick"],
        "stats_region_end_tick": run["stats_region_end_tick"],
        "rgba_sha256": run["sha256"],
        "analytic_max_byte_error": run["analytic_max_byte_error"],
        "wall_seconds": run["wall_seconds"],
        "command": run["command"],
        "idle_machine_checked": run["idle_machine_checked"],
        "process_checks": run["process_checks"],
        "simulations_before": run["simulations_before"],
        "simulations_after": run["simulations_after"],
        "repeat": run["repeat"],
        "artifact_directory": relative(directory),
        "raw_artifact_sha256": {
            name: digest(directory / name)
            for name in (
                "stats.txt",
                "config.json",
                "parameters.json",
                "wall.json",
                "run.log",
                "validation.json",
            )
        },
        "output_sha256": {
            name: digest(directory / name)
            for name in (
                "image.rgba",
                "image.bmp",
                "scene.bin",
                "job.bin",
                "parameters.bin",
                "core_count.bin",
                *(f"job{i}.bin" for i in range(len(cores))),
            )
        },
    }


def summarize(runs, compared=None):
    assert len(runs) == 1
    wall = runs[0]["wall_seconds"]
    result = {
        key: value
        for key, value in runs[0].items()
        if key
        not in (
            "command",
            "repeat",
            "raw_artifact_sha256",
            "wall_seconds",
            "process_checks",
            "simulations_before",
            "simulations_after",
            "measurement_utc",
            "artifact_directory",
            "full_simulated_cycles_per_wall_second",
        )
    }
    result.update(
        samples=1,
        wall_seconds=wall,
        artifact_directories=[run["artifact_directory"] for run in runs],
        matched_simulated_counters=compared,
    )
    result["full_simulated_cycles_per_wall_second"] = (
        result["full_simulated_cycles"] / wall
    )
    return result


def collect(args):
    suite = load(args.evidence / "suite.json")
    assert suite.get("finished_utc"), "Measurement suite has not finished"
    assert all(entry["returncode"] == 0 for entry in suite["commands"])
    groups, runs, configurations, comparisons = [], [], [], []
    for count in (8, 16):
        directory = args.evidence / f"accel{count}"
        group = load(directory / "validation.json")
        assert group["timing_only"] and group["performance_verified_idle"]
        assert group["quantum"] == group["transport_delay"] == "5ns"
        assert group["accelerator_cpu"] == "o3"
        assert group["memory_controllers"] == 4
        assert len(group["runs"]) == 2
        group_runs = []
        smoke = compare(
            directory / "q1-0", directory / f"q{count + 1}-0", traces=False
        )
        for run in group["runs"]:
            assert run["repeat"] == 0
            path = directory / f"q{run['queues']}-{run['repeat']}"
            config = load(path / "config.json")
            parameters = load(path / "parameters.json")
            assert config["sim_quantum"] == run["quantum_ticks"] == 5000
            assert parameters["transport_delay"] == "5ns"
            assert parameters["trace"] is False
            assert parameters["accelerator_caches"] == "l1"
            assert parameters["queue_depth"] == 16
            assert parameters["dma_outstanding"] == 256
            assert run["sha256"] == GOLDEN
            assert run["idle_machine_checked"] and run["process_checks"] > 0
            metric = accelerator_metrics(run, path)
            runs.append(metric)
            group_runs.append(metric)
        for queues in (1, count + 1):
            configurations.append(
                summarize(
                    [run for run in group_runs if run["queues"] == queues],
                    smoke["simulated_counters_compared"],
                )
            )
        comparisons.append(
            {
                "accelerators": count,
                "smoke": smoke,
                "protocol": group["protocol"],
                "validation_sha256": digest(directory / "validation.json"),
            }
        )
        assert len(group["protocol"]["transaction_trace_sha256"]) == (
            2 * count + 1
        )
        groups.append(group)
    a, b = groups
    for key in ("gem5_sha256", "firmware_sha256", "source_sha256"):
        assert a[key] == b[key], key
    assert suite["artifact_sha256"]["build/RISCV/gem5.opt"] == a["gem5_sha256"]
    assert digest(args.evidence / "artifacts/gem5.opt") == a["gem5_sha256"]
    host_runs = []
    for repeat in range(1):
        directory = args.evidence / f"host-{repeat}"
        host = load(directory / "validation.json")
        assert host["gem5_sha256"] == a["gem5_sha256"]
        assert host["firmware_sha256"]["accelerator.elf"] == (
            a["firmware_sha256"]["accelerator.elf"]
        )
        assert host["quantum_ticks"] == 5000
        assert host["transport_delay"] == "5ns"
        assert host["rgba_sha256"] == GOLDEN
        assert host["idle_machine_checked"] and host["process_checks"] > 0
        for name, value in a["source_sha256"].items():
            assert host["source_sha256"][name] == value, name
        host["repeat"] = repeat
        host_runs.append(host)
        runs.append(host)
    configurations.append(summarize(host_runs))
    for config in configurations[:-1]:
        serial = next(
            row
            for row in configurations
            if row["renderer"] == "Accelerators"
            and row["accelerators"] == config["accelerators"]
            and row["queues"] == 1
        )
        config["pdes_wall_speedup"] = (
            serial["wall_seconds"] / config["wall_seconds"]
        )
    configurations[-1]["pdes_wall_speedup"] = None
    source_sha = {**a["source_sha256"], **host_runs[0]["source_sha256"]}
    for name, value in source_sha.items():
        assert digest(ROOT / name) == value, name
    return {
        "workload": {
            "width": 96,
            "height": 96,
            "mode": 1,
            "samples_per_pixel": 1,
            "max_secondary_depth": 0,
            "seed": 1,
        },
        "samples_per_configuration": 1,
        "accelerator_cpu": "o3",
        "accelerator_caches": "l1",
        "memory_controllers": 4,
        "quantum": "5ns",
        "quantum_ticks": 5000,
        "transport_delay": "5ns",
        "source_sha256": source_sha,
        "gem5_sha256": a["gem5_sha256"],
        "firmware_sha256": {
            **a["firmware_sha256"],
            **host_runs[0]["firmware_sha256"],
        },
        "base_commit": suite["git_commit"],
        "measurement_utc": {
            "started": suite["started_utc"],
            "finished": suite["finished_utc"],
        },
        "machine": a["machine"],
        "runs": runs,
        "configurations": configurations,
        "queue_pair_comparisons": comparisons,
        "raw_validation_directories": [
            relative(args.evidence / f"accel{count}") for count in (8, 16)
        ],
        "suite": suite,
        "report_generator_sha256": digest(Path(__file__)),
        "host_baseline": {
            "modeled_speedup_by_accelerator_count": {
                str(count): configurations[-1]["batch_simulated_cycles"]
                / configurations[index]["batch_simulated_cycles"]
                for index, count in ((0, 8), (2, 16))
            }
        },
    }


def collect_transport_sweep(result, directory):
    sweep = load(directory / "sweep.json")
    assert sweep["finished_utc"], "Transport sweep has not finished"
    assert sweep["samples_per_configuration"] == 1
    assert sweep["accelerators"] == 8 and sweep["queues"] == 9
    assert len(sweep["runs"]) == len(sweep["quantums_ns"])
    assert sweep["gem5_sha256"] == result["gem5_sha256"]
    assert sweep["source_sha256"].items() <= result["source_sha256"].items()
    for name, sha in sweep["firmware_sha256"].items():
        assert result["firmware_sha256"][name] == sha, name
    runner = ROOT / sweep.get(
        "runner_artifact", "util/rt_reference/measure_transport.py"
    )
    assert digest(runner) == (sweep["runner_sha256"])
    assert (
        sweep["baseline_directory"] == result["raw_validation_directories"][0]
    )
    assert sweep["baseline_validation_sha256"] == (
        result["queue_pair_comparisons"][0]["validation_sha256"]
    )
    for name in ("hostname", "platform", "cpu_affinity", "pid_namespace"):
        assert sweep["machine"][name] == result["machine"][name], name
    serial = result["configurations"][0]
    assert sweep["speedup_baseline_transport_delay"] == "5ns"
    assert sweep["speedup_baseline_wall_seconds"] == serial["wall_seconds"]
    for quantum, run in zip(sweep["quantums_ns"], sweep["runs"]):
        path = directory / f"{quantum}ns/q9-0"
        assert run["artifact_directory"] == relative(path)
        assert run["queues"] == 9 and run["accelerator_cores"] == 8
        assert run["repeat"] == 0
        assert run["transport_delay"] == f"{quantum}ns"
        assert (
            check_transport(load(path / "config.json"), f"{quantum}ns")
            == (run["quantum_ticks"])
            == 1000 * quantum
        )
        assert load(path / "parameters.json")["transport_delay"] == (
            f"{quantum}ns"
        )
        if run.get("status") == "failed":
            assert run["returncode"] != 0
            assert run["wall_seconds"] is None
            assert load(path / "failure.json")["returncode"] == (
                run["returncode"]
            )
            metric = {
                **run,
                "renderer": "Accelerators",
                "accelerators": 8,
                "memory_controllers": 4,
                "pdes_wall_speedup": None,
                "raw_artifact_sha256": {
                    name: digest(path / name)
                    for name in (
                        "config.json",
                        "parameters.json",
                        "run.log",
                        "stats.txt",
                        "failure.json",
                    )
                },
            }
            result["runs"].append(metric)
            result["configurations"].append(
                {
                    **metric,
                    "samples": 0,
                    "attempts": 1,
                    "artifact_directories": [relative(path)],
                    "matched_simulated_counters": None,
                }
            )
            continue
        assert run["sha256"] == GOLDEN
        wall = load(path / "wall.json")
        assert wall["wall_seconds"] == run["wall_seconds"]
        assert wall["command"] == run["command"]
        assert run["idle_machine_checked"] and run["process_checks"] > 0
        assert run["analytic_max_byte_error"] <= 1
        metric = accelerator_metrics(run, path)
        metric.update(
            started_utc=run["started_utc"],
            finished_utc=run["finished_utc"],
        )
        result["runs"].append(metric)
        config = summarize([metric])
        config["pdes_wall_speedup"] = (
            serial["wall_seconds"] / config["wall_seconds"]
        )
        assert config["pdes_wall_speedup"] == run["speedup_vs_5ns_serial"]
        config["speedup_baseline_transport_delay"] = "5ns"
        config["speedup_baseline_artifact_directory"] = serial[
            "artifact_directories"
        ][0]
        result["configurations"].append(config)
    for key in ("quantum", "quantum_ticks", "transport_delay"):
        result[f"baseline_{key}"] = result[key]
    result["timing_parameter_scope"] = (
        "Top-level timing fields describe the 5 ns baseline; each run and "
        "configuration records its own transport delay and quantum."
    )
    result["quantums_ns"] = [5, *sweep["quantums_ns"]]
    result["successful_samples"] = sum(
        run.get("status") != "failed" for run in result["runs"]
    )
    result["failed_attempts"] = sum(
        run.get("status") == "failed" for run in result["runs"]
    )
    result["transport_sweep"] = {
        **sweep,
        "evidence_directory": relative(directory),
        "sweep_sha256": digest(directory / "sweep.json"),
        "speedup_definition": (
            "5 ns, eight-accelerator, one-queue process wall divided by "
            "the measured nine-queue process wall at each transport delay"
        ),
        "same_delay_serial_samples": False,
    }


def collect_transport_retry(result, directory):
    # Reuse the same provenance/configuration checks for the separate retry.
    retry = copy.deepcopy(result)
    retry["runs"] = retry["runs"][:5]
    retry["configurations"] = retry["configurations"][:5]
    collect_transport_sweep(retry, directory)
    assert retry["transport_sweep"]["quantums_ns"] == [1000]
    replacement = retry["configurations"][-1]
    assert replacement.get("status") != "failed"
    replaced = next(
        index
        for index, row in enumerate(result["configurations"])
        if row.get("status") == "failed"
        and row["quantum_ticks"] == replacement["quantum_ticks"]
    )
    original = result["configurations"][replaced]
    replacement["attempts"] = 2
    replacement["prior_failed_artifact_directories"] = original[
        "artifact_directories"
    ]
    result["configurations"][replaced] = replacement
    result["runs"][replaced] = retry["runs"][-1]
    result["successful_samples"] += 1
    result["failed_attempts"] -= 1
    result["transport_sweep"]["successful_retry"] = retry["transport_sweep"]
    result["historical_failed_attempts"] = 1


def report(result):
    configs = result["configurations"]
    rows = []
    for row in [*configs[:2], *configs[5:], *configs[2:5]]:
        if row.get("status") == "failed":
            columns = [
                row["renderer"],
                str(row["accelerators"]),
                str(row["queues"]),
                f"{row['quantum_ticks'] / 1000:g}",
                "4",
                f"Failed ({row['signal'] or row['returncode']})",
                *["—"] * 18,
            ]
            rows.append("| " + " | ".join(columns) + " |")
            continue
        host = row["renderer"] == "Host"
        count = f"{row['accelerators']} (WFI)" if host else row["accelerators"]
        speedup = row["pdes_wall_speedup"]
        speedup = "—" if speedup is None else f"{speedup:.3f}x"
        icache = row["cache"]["icache"]
        ihits = "—" if icache is None else f"{icache['hit_percent']:.3f}"
        dcache = row["cache"]["dcache"]
        compared = row["matched_simulated_counters"]
        compared = "—" if compared is None else f"{compared:,}"
        rows.append(
            f"| {row['renderer']} | {count} | {row['queues']} | "
            f"{row['quantum_ticks'] / 1000:g} | 4 | "
            f"{row['wall_seconds']:.3f} | {speedup} | "
            f"{row['full_simulated_cycles']:,.0f} | "
            f"{row['batch_simulated_cycles']:,.0f} | "
            f"{row['batch_retired_instructions']:,} | "
            f"{row['batch_counted_cpu_cycles']:,} | "
            f"{row['weighted_core_ipc']:.6f} | {row['cluster_ipc']:.6f} | "
            f"{row['full_simulated_cycles_per_wall_second']:,.0f} | "
            f"{row['batch_megapixels_per_simulated_second']:.3f} | "
            f"{row['counted_cpu_occupancy_percent']:.2f} | {ihits} | "
            f"{dcache['hit_percent']:.3f} | "
            f"{dcache['mshr_miss_percent']:.3f} | "
            f"{row['dma_batch_duty_percent']:.3f} | "
            f"{row['dma_copied_bytes'] / 1024:.3f} | "
            f"{row['ram_read_bytes'] / 1024:.3f} | "
            f"{row['ram_write_bytes'] / 1024:.3f} | {compared} |"
        )
    return build_markdown(result, rows)


def build_markdown(result, rows):
    s8, p8, s16, p16, host = result["configurations"][:5]
    host_speedup = result["host_baseline"][
        "modeled_speedup_by_accelerator_count"
    ]
    wfi = host["idle_accelerators"][0]["wfi_suspend_tick"]
    comparisons = result["queue_pair_comparisons"]
    evidence = result["runs"][0]["artifact_directory"].rsplit("/accel8/", 1)[0]
    count_ratio = p8["wall_seconds"] / p16["wall_seconds"]
    if count_ratio < 1:
        count_note = (
            f"On this host, sixteen accelerators took "
            f"{100 * (1 / count_ratio - 1):.2f}% more process wall time than "
            "eight in PDES, despite the shorter modeled batch."
        )
    else:
        count_note = (
            f"On this host, sixteen accelerators used "
            f"{100 * (1 - 1 / count_ratio):.2f}% less process wall time than "
            "eight in PDES."
        )
    sweep = result.get("transport_sweep")
    sweep_intro = sweep_note = sweep_evidence = sweep_reproduction = ""
    if sweep:
        retry = sweep.get("successful_retry")
        retry_option = ""
        quantums = ", ".join(str(value) for value in sweep["quantums_ns"])
        successful = [
            row
            for row in result["configurations"][5:]
            if row.get("status") != "failed"
        ]
        best = min(
            [p8, *successful],
            key=lambda row: row["wall_seconds"],
        )
        largest = max([p8, *successful], key=lambda row: row["quantum_ticks"])
        largest_batch_change = 100 * (
            largest["batch_simulated_cycles"] / p8["batch_simulated_cycles"]
            - 1
        )
        sweep_intro = (
            f"\nThe eight-accelerator, nine-queue transport sweep ran from "
            f"`{sweep['started_utc']}` through `{sweep['finished_utc']}`, "
            f"with **one attempt each at {quantums} ns**. At 1 GHz these are "
            "also CPU clocks. Each value sets both `Root.sim_quantum` and "
            "each direction's transport delay. The other settings, "
            "executable and firmware match the 5 ns baseline.\n"
        )
        if retry:
            sweep_intro += (
                "\nThe requested **1000 ns retry without GDB** ran from "
                f"`{retry['started_utc']}` through "
                f"`{retry['finished_utc']}` and passed. The table uses "
                "its single validated timing sample for the 1000 ns row.\n"
            )
            retry_option = (
                f"    --transport-retry={retry['evidence_directory']} " "\\\n"
            )
        sweep_note = (
            f"\nAll eight-accelerator speedups use the existing "
            f"**{s8['wall_seconds']:.3f} s, "
            "5 ns, one-queue baseline**. The sweep contains six new "
            "nine-queue runs; serial runs at the larger delays were not "
            "measured. Changing transport delay changes modeled latency "
            "as well as synchronization frequency, so the new speedups "
            "compare these configurations against the fixed baseline. "
            "Equal counters is blank for the new rows because no "
            "same-delay queue pair was run.\n\n"
            f"The fastest measured eight-accelerator configuration uses "
            f"**{best['quantum_ticks'] / 1000:g} ns**, taking "
            f"**{best['wall_seconds']:.3f} s**: "
            f"**{best['pdes_wall_speedup']:.3f}x** versus serial and "
            f"**{p8['wall_seconds'] / best['wall_seconds']:.3f}x** versus "
            f"5 ns PDES. At {largest['quantum_ticks'] / 1000:g} ns, "
            "modeled batch clocks change "
            f"by {largest_batch_change:.3f}% "
            "relative to 5 ns. These are single observations.\n"
        )
        for row in sweep["runs"]:
            if row.get("status") == "failed":
                sweep_note += (
                    f"\nThe original **{row['quantum_ticks'] / 1000:g} ns attempt "
                    f"failed with {row['signal']}** (return code "
                    f"{row['returncode']}) during rendering. The backtrace "
                    "points to `gem5::o3::Commit::tick()` at "
                    "`src/cpu/o3/commit.cc:648`, the commit-stall probe "
                    "notification. No statistics dump or image was "
                    "produced; its wall time and speedup are unavailable. "
                    "The failure site does not establish the root cause.\n"
                )
                if retry:
                    sweep_note += (
                        "\nA separate GDB diagnostic and the later timed "
                        "retry without GDB both completed with the "
                        "expected image. Only the timed retry contributes "
                        "to the table. The original crash remains in "
                        "the raw and machine-readable evidence.\n"
                    )
        sweep_evidence = (
            f"\nThe six transport-sweep directories are under "
            f"`{sweep['evidence_directory']}`, one `<delay>ns/q9-0` per "
            "quantum. `sweep.json` records measured wall times, commands, "
            "per-run UTC timestamps, host PID namespace and unchanged "
            "binary/source/firmware hashes. All successful images and serialized "
            "scene/job records match the 5 ns reference. Deterministic "
            "timing counters may differ across transport delays.\n"
        )
        if retry:
            sweep_evidence += (
                f"\nThe successful 1000 ns timing sample is under "
                f"`{retry['evidence_directory']}/1000ns/q9-0`; its "
                "`sweep.json` records the same provenance and idle-machine "
                "checks as the original sweep. No failed or "
                "debugger-assisted run is averaged into its wall time.\n"
            )
        sweep_reproduction = f"""
Run exactly one nine-queue sample per requested transport delay, reusing the
completed eight-core baseline:

```sh
python3 util/rt_reference/measure_transport.py \\
    --baseline={evidence}/accel8 \\
    --output=m5out/rt_backend/raytrace-transport-new \\
    --quantums 10 20 50 100 200 1000
```

Add `--inspect-only` to recheck the saved sweep without simulations. Include
the measured sweep when regenerating the table:

```sh
python3 util/rt_reference/report_raytrace.py \\
    --evidence={evidence} \\
    --transport-sweep={sweep['evidence_directory']} \\
{retry_option}\
    --output-json=util/rt_reference/RAYTRACE_RESULTS.json \\
    --output-md=util/rt_reference/RAYTRACE_RESULTS.md
```
"""
    return f"""# Ray-tracing results: four RAM controllers and private L1 caches

Remeasured from `{result['measurement_utc']['started']}` through
`{result['measurement_utc']['finished']}` using the current **5 ns transport
delay in each direction and 5 ns quantum**. The validator checks
`Root.sim_quantum = 5,000` ticks and every wrapper's request/response delay
in both queue modes. The tick period is 1 ps; CPU/device clocks are 1 GHz.
{sweep_intro}

These results cover the **96x96 smoke test only**: mode 1, one sample per
pixel, maximum secondary depth 0, seed 1. Each completed configuration
has **one process-wall sample**. All accelerator
CPUs use `RiscvO3CPU`; the rendering example's general CPU default is `timing`.

At 5 ns, queue speedup is **{p8['pdes_wall_speedup']:.3f}x for eight accelerators** and
**{p16['pdes_wall_speedup']:.3f}x for sixteen accelerators**. Doubling the
accelerator count reduces modeled batch time by
{s8['batch_simulated_cycles'] / s16['batch_simulated_cycles']:.3f}x; the
9-queue/17-queue process-wall ratio is
{count_ratio:.3f}x. Queue count
changes simulation execution only: every compared modeled counter and image
agrees within each core-count pair. These single samples do not establish
wall-time variance.

{count_note}

| Renderer | Accelerators present | Event queues | Quantum / transport (ns) | RAM controllers | Process wall (s) | Speedup vs 5 ns DES | Full clocks | Batch clocks | Retired inst. | CPU clocks sum | Core IPC | Cluster IPC | Simulated clocks/s | Modeled MPixel/s | CPU occupancy (%) | L1I hits (%) | D/unified cache hits (%) | D/unified MSHR misses (%) | DMA duty (%) | DMA copied (KiB) | RAM read (KiB) | RAM written (KiB) | Equal counters |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
{chr(10).join(rows)}
{sweep_note}

The host baseline renders directly on one host CPU, with all eight
accelerator CPUs in WFI and no submitted jobs. Its modeled render interval
is {host['batch_simulated_cycles']:,.0f} clocks; eight and sixteen active
accelerators are {host_speedup['8']:.3f}x and {host_speedup['16']:.3f}x faster
in modeled time. These are separate from simulator wall-time speedups.

The original five samples use eight-core DES/PDES, sixteen-core DES/PDES,
then the host baseline. Every simulation ran sequentially. Separate small
traced protocol pairs run before each core-count group's timing samples and
are excluded from wall-time results. Additional repeats collected before the
request was narrowed to one sample per configuration are retained in the raw
directory but excluded from every reported metric.

## Measured configuration

Four independent native `SimpleMemory` controllers cover the 64 MiB host
aperture at `0x80000000`, with 64-byte interleaving using address bits 6 and
7. Each has 30 ns response latency and 12.8 GiB/s configured bandwidth,
quantized to 73 ps/byte. The shared host crossbar is 16 bytes wide at 1 GHz
with one frontend, forward and response cycle. These rendering defaults
differ from the reduction memory configuration.

Each accelerator has an 8 KiB, two-way L1I with four MSHRs and a 16 KiB,
four-way L1D with sixteen MSHRs and sixteen write buffers. Cache tag, data
and response latencies are one 1 GHz cycle; cache lines are 64 bytes and
there is no prefetcher. Coherent DMA cleans and invalidates destination
lines before writes and snoops dirty CPU data on reads. The private
coherent crossbar is 64 bytes wide at 4 GHz, with zero added frontend,
forward and response stages. Code TCM is 64 KiB, read-only, with independent
1 ns instruction/constant ports; Data TCM is 192 KiB at 1 ns and 128 GiB/s.
Host RAM and MMIO remain uncached by accelerator CPUs.

The host uses one 1 GHz `RiscvO3CPU` and a unified 64 KiB, eight-way cache.
Accelerator CPUs are scalar RV64IMAFDC, one hart each, at 1 GHz, with
standard O3 resources: width 8, 192 ROB entries and 32-entry load/store
queues. DMA has 256 outstanding chunks and transport queue depth is 16.
The original five rows use **5 ns** request and response transport stages;
the automatically derived PDES quantum is **5 ns in both modes**. The sweep
varies all three delays together. DES places all objects on
queue 0. PDES keeps the host, RAM and adapters on queue 0 and places each
accelerator system on its own queue.

Eight accelerators render a 2x4 grid of 48x24 tiles; sixteen render a 4x4
grid of 24x24 tiles. Both cover all 9,216 pixels exactly once. Each core
DMA-reads a 56-byte job and an 11,368-byte scene and DMA-writes its tile;
aggregate output is 36,864 bytes in every run.

## Metric definitions

Process wall time includes launch, boot, rendering, statistics dumps,
image export and shutdown. PDES speedup divides the one-queue sample by the
multi-queue sample at the same accelerator count, using the fixed 5 ns serial
baseline for the transport sweep. Full clocks are the final
simulation tick divided by the 1,000-tick CPU period. Simulated clocks/s
divides full clocks by process wall time.

Batch metrics use the first statistics dump. Accelerator firmware resets
statistics after scene/descriptor preparation, quiesces past the deferred
reset, then submits jobs. It dumps after all completions and terminal status
checks. The interval includes descriptor/scene DMA, setup, rendering, output
DMA, MSI and boundary synchronization gaps. Boot/preparation and image export
are excluded; the second dump confirms zero accelerator execution during
export. Batch clocks are `simTicks / 1000`.

Retired instructions sum accelerator `cpu.commitStats0.numInsts`; counted
CPU clocks sum accelerator `cpu.numCycles`. Core IPC divides total retired
instructions by counted CPU clocks; cluster IPC uses common batch clocks.
Modeled MPixel/s is 9,216 divided by modeled batch seconds and one million.
CPU occupancy is summed counted clocks divided by accelerator count times
batch clocks, a counted-cycle proxy. Host counters are excluded from
accelerator aggregates. The Host row measures only the host CPU; its core
IPC uses counted cycles and cluster IPC uses the render interval.

Cache percentages aggregate hits and misses across active renderer caches.
The Host row uses the unified host cache in the D/unified columns. L1D
misses include requests merged into an outstanding MSHR. MSHR-miss percentage
uses `overallMshrMisses / (overallHits + overallMisses)` to distinguish new
miss transactions. DMA duty is summed `control.busyTicks` divided by
accelerator count times batch ticks; it includes waits, coherence and drain
and can overlap CPU work. Copied bytes count completed transfers. RAM bytes
sum all four controllers, including host traffic in the batch. KiB is 1,024
bytes. Equal counters counts both dumps for a queue pair.

## Correctness and measurement evidence

All **{result.get('successful_samples', len(result['runs']))} successful timing runs** produce the golden RGBA SHA-256:
`{GOLDEN}`.
BMP pixels agree with RGBA; the independent double-precision lighting
reference differs by at most one byte. At 5 ns and each accelerator count, scene,
descriptors, parameters, core count, RGBA and BMP are byte-identical between
queue modes. The host baseline produces the same image.
All deterministic counters in both dumps agree within each 5 ns queue pair.
Host execution-rate,
wall-time and process-memory counters are excluded.

The 5 ns eight-core protocol pair matches
{comparisons[0]['protocol']['simulated_counters_compared']:,} modeled counters
and all 17 traces; the sixteen-core pair matches
{comparisons[1]['protocol']['simulated_counters_compared']:,} counters and all
33 traces. These check discovery, repeated submissions, error recovery,
DMA/TCM range checks, completion visibility and backpressure. Full-scene
timing runs omit transaction tracing. The host run adds only sparse `Quiesce`
logging, included in its process wall time.

The host PID namespace guard found no competing gem5 processes before,
during or after timed runs. CPU affinity remained unrestricted over CPUs
0–19. Host: `{result['machine']['hostname']}`, Intel Core Ultra 7 265K,
20 cores/threads (8 performance and 12 efficiency). Platform:
`{result['machine']['platform']}`. OS scheduling, turbo and ordinary desktop
activity remain enabled; the guard establishes absence of competing gem5
processes, not an otherwise idle desktop.

[Machine-readable evidence](RAYTRACE_RESULTS.json) contains every sample,
full-precision aggregate/per-core metrics, controller traffic, commands,
guard counts, comparisons and source/executable/firmware hashes. Raw results
are under `{evidence}`: `accel8/q1-0`, `accel8/q9-0`, `accel16/q1-0`,
`accel16/q17-0`, and `host-0`. Each core-count directory also
contains its new protocol evidence. `artifacts/` preserves the measured
executable, firmware, source patch and initial git
status. Previous 1 us raw directories are retained as historical evidence
and do not contribute to these results.
{sweep_evidence}

gem5 was rebuilt from current source without temporary profiling
instrumentation. All runs use executable SHA-256:
`{result['gem5_sha256']}`.
Base commit: `{result['base_commit']}`. Accelerator measurements share the
same firmware; the host uses `host-render.elf`, built from the same scalar
renderer with render-region statistics enabled.

## Host baseline and WFI evidence

The host prepares the scene and whole-image descriptor, resets statistics,
waits past the deferred reset, renders directly, then dumps/reset statistics
before image export. It submits no accelerator jobs. All eight accelerator
CPUs suspend at tick {wfi:,}, before the host render region starts at tick
{host['stats_region_start_tick']:,}, and have no wakeups. Both dumps show zero
accelerator instructions, counted cycles, DMA requests/copied bytes, TCM
traffic and adapter RAM traffic. `wfi.log` records the
suspend events. Host cache and RAM timings match the accelerator cases.

## Reproduction

From the repository root, choose a fresh output directory. The rendering
memory/cache defaults are exercised without overrides.

```sh
scons -Q build/RISCV/gem5.opt -j 8
make -C util/rt_reference all backend-tests out/native out/host-render.elf
python3 util/rt_reference/validate_backend.py --accelerator-cores=8 \\
    --accelerator-cpu=o3 --runs=1 --timing-only --require-idle-machine \\
    --transport-delay=5ns --output=m5out/rt_backend/raytrace-5ns/accel8
python3 util/rt_reference/validate_backend.py --accelerator-cores=16 \\
    --accelerator-cpu=o3 --runs=1 --timing-only --require-idle-machine \\
    --transport-delay=5ns --output=m5out/rt_backend/raytrace-5ns/accel16
python3 util/rt_reference/measure_host_render.py \\
    --reference=m5out/rt_backend/raytrace-5ns/accel8/validation.json \\
    --transport-delay=5ns --output=m5out/rt_backend/raytrace-5ns/host-0
```

Run measurements in the host PID namespace so the process guard sees all
simulations. Add `--inspect-only` to measurement commands to recheck saved
outputs without rerunning. Regenerate both reports from this completed suite:

```sh
python3 util/rt_reference/report_raytrace.py \\
    --evidence={evidence} \\
    --output-json=util/rt_reference/RAYTRACE_RESULTS.json \\
    --output-md=util/rt_reference/RAYTRACE_RESULTS.md
```
{sweep_reproduction}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--transport-sweep", type=Path)
    parser.add_argument("--transport-retry", type=Path)
    parser.add_argument("--output-json", type=Path, required=True)
    parser.add_argument("--output-md", type=Path, required=True)
    args = parser.parse_args()
    result = collect(args)
    if args.transport_sweep:
        collect_transport_sweep(result, args.transport_sweep)
    if args.transport_retry:
        assert args.transport_sweep, "A retry needs its original sweep"
        collect_transport_retry(result, args.transport_retry)
    args.output_json.write_text(json.dumps(result, indent=2) + "\n")
    lines, paragraph = [], []
    in_code = False

    def flush():
        if paragraph:
            lines.append(
                textwrap.fill(
                    " ".join(paragraph),
                    width=79,
                    break_long_words=False,
                    break_on_hyphens=False,
                )
            )
            paragraph.clear()

    for line in report(result).splitlines():
        if (
            not in_code
            and not line
            and not paragraph
            and lines
            and not lines[-1]
        ):
            continue
        if line.startswith("```"):
            flush()
            in_code = not in_code
            lines.append(line)
        elif in_code or not line or line.startswith(("#", "|")):
            flush()
            lines.append(line)
        else:
            paragraph.append(line.strip())
    flush()
    args.output_md.write_text("\n".join(lines).rstrip() + "\n")
    print(
        f"PASS: {result.get('successful_samples', len(result['runs']))} "
        f"timing samples, failed attempts: "
        f"{result.get('failed_attempts', 0)}, "
        "both protocol pairs "
        "and per-row transport/quantum checks"
    )


if __name__ == "__main__":
    main()
