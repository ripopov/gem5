#!/usr/bin/env python3
"""Serial five-run timings of the finalists and their strongest controls.

Each variant is built once and launched --repeat times in fresh simulators,
one at a time, so launcher wall times are comparable. The summary reports
median and min-max GPU-active cycles per variant.
"""

import argparse
import json
import statistics
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
WORKSPACE = HERE.parents[1]
TUNE = WORKSPACE / "experiments/gemm-study/tune.py"
LDS = [
    "TILE_M=64",
    "TILE_N=32",
    "WAVE_M=64",
    "WAVE_N=16",
    "PREFETCH_LDS=2",
    "LDS_PAD=16",
]
REG = ["TILE_M=64", "TILE_N=32", "WAVE_M=32", "WAVE_N=32"]

FINALISTS = {
    # 1. conventional nonpersistent tiled GEMM, single and double buffered
    "nonpersist-p1": ("reference-lds", LDS + ["PERSISTENT=0", "PIPELINE=1"]),
    "nonpersist-p2": ("reference-lds", LDS + ["PERSISTENT=0", "PIPELINE=2"]),
    # 2. cooperative persistent GEMM (the sample defaults to the specialised
    #    pipeline, so the producer count must be forced to zero)
    "coop-s1-w8": (
        "reference-lds",
        LDS + ["PRODUCERS=0", "GROUPS=1", "STAGES=1", "WORKGROUPS=8"],
    ),
    "coop-s2-w8": (
        "reference-lds",
        LDS + ["PRODUCERS=0", "GROUPS=1", "STAGES=2", "WORKGROUPS=8"],
    ),
    "coop-s1-w16": (
        "reference-lds",
        LDS + ["PRODUCERS=0", "GROUPS=1", "STAGES=1", "WORKGROUPS=16"],
    ),
    # 3. producer + compute/store specialisation, two groups and one group
    "spec-p128-g2-s4-f1-w8": (
        "reference-lds",
        LDS
        + [
            "PRODUCERS=128",
            "GROUPS=2",
            "STAGES=4",
            "PREFETCH=1",
            "WORKGROUPS=8",
        ],
    ),
    "spec-p128-g2-s2-f1-w8": (
        "reference-lds",
        LDS
        + [
            "PRODUCERS=128",
            "GROUPS=2",
            "STAGES=2",
            "PREFETCH=1",
            "WORKGROUPS=8",
        ],
    ),
    "spec-p128-g2-s6-f2-w8": (
        "reference-lds",
        LDS
        + [
            "PRODUCERS=128",
            "GROUPS=2",
            "STAGES=6",
            "PREFETCH=2",
            "WORKGROUPS=8",
        ],
    ),
    "spec-p128-g1-s4-f2-w8": (
        "reference-lds",
        LDS
        + [
            "PRODUCERS=128",
            "GROUPS=1",
            "STAGES=4",
            "PREFETCH=2",
            "WORKGROUPS=8",
        ],
    ),
    "spec-p128-g2-s4-f1-w16": (
        "reference-lds",
        LDS
        + [
            "PRODUCERS=128",
            "GROUPS=2",
            "STAGES=4",
            "PREFETCH=1",
            "WORKGROUPS=16",
        ],
    ),
    "spec-p128-g1-s4-f2-w16": (
        "reference-lds",
        LDS
        + [
            "PRODUCERS=128",
            "GROUPS=1",
            "STAGES=4",
            "PREFETCH=2",
            "WORKGROUPS=16",
        ],
    ),
    # 5. cache/register only
    "reg-persist-p1-w8": (
        "reference-regtiles",
        REG + ["PIPELINE=1", "WORKGROUPS=8"],
    ),
    "reg-nonpersist-p1": (
        "reference-regtiles",
        REG + ["PERSISTENT=0", "PIPELINE=1"],
    ),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shape", nargs=3, type=int, required=True)
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--prefix", default="final-")
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument(
        "--config-arg", action="append", default=["--reg-alloc-policy=dynamic"]
    )
    parser.add_argument(
        "--naive", action="store_true", help="also time the naive sample once"
    )
    parser.add_argument("--summary", action="store_true")
    args = parser.parse_args()
    names = [n for n in FINALISTS if not args.only or n in args.only]
    if not args.summary:
        jobs = [(n, *FINALISTS[n], args.repeat) for n in names]
        if args.naive:
            jobs.append(("naive", "naive", [], 1))
        for name, kind, defines, repeat in jobs:
            command = [
                sys.executable,
                str(TUNE),
                args.prefix + name,
                kind,
                "--shape",
                *map(str, args.shape),
                "--repeat",
                str(repeat),
            ]
            for d in defines:
                command += ["--define", d]
            for c in args.config_arg:
                command.append("--config-arg=" + c)
            print("START " + args.prefix + name, flush=True)
            subprocess.run(command, cwd=WORKSPACE, check=False)
    rows = {}
    for directory in sorted(
        (WORKSPACE / "work/gemm-study").glob(args.prefix + "*")
    ):
        cycles, walls, hosts, runs = [], [], [], []
        results = [
            json.loads(p.read_text())
            for p in sorted(directory.glob("result-*.json"))
        ]
        # Only the most recent build of the variant counts.
        latest = results[-1]["binary_sha256"] if results else None
        for r in results:
            if (
                r.get("gpu_active_cycles") is None
                or r["binary_sha256"] != latest
            ):
                continue
            cycles.append(r["gpu_active_cycles"])
            walls.append(r["wall_seconds"])
            hosts.append(r["host_seconds"])
            runs.append(r["run"])
        if cycles:
            rows[directory.name] = {
                "binary_sha256": latest,
                "defines": results[-1]["defines"],
                "config_args": results[-1].get("config_args"),
                "runs": runs,
                "cycles": cycles,
                "median_cycles": statistics.median(cycles),
                "min_cycles": min(cycles),
                "max_cycles": max(cycles),
                "median_wall_seconds": statistics.median(walls),
                "median_host_seconds": statistics.median(hosts),
            }
    for name, row in sorted(
        rows.items(), key=lambda kv: kv[1]["median_cycles"]
    ):
        print(
            f"{name:45s} median {round(row['median_cycles']):>8} "
            f"range {round(row['min_cycles'])}-{round(row['max_cycles'])} "
            f"n={len(row['cycles'])} wall {row['median_wall_seconds']:.1f}s"
        )
    (
        WORKSPACE / "work/gemm-persist" / (args.prefix + "summary.json")
    ).write_text(json.dumps(rows, indent=2) + "\n")


if __name__ == "__main__":
    main()
