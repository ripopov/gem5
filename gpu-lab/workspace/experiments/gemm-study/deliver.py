#!/usr/bin/env python3
"""Reproduce final GEMM timings, correctness controls, and pipeline traces."""

import argparse
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--phase",
        choices=("all", "timings", "checks", "traces"),
        default="all",
    )
    parser.add_argument("--repeat", type=int, default=5)
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("repeat must be positive")
    jobs = []
    if args.phase in ("all", "timings"):
        jobs += [
            ("lds", "reference-lds", [], 0, args.repeat, False),
            (
                "single",
                "reference-lds",
                ["PIPELINE=1", "LDS_PAD=0"],
                0,
                args.repeat,
                False,
            ),
            (
                "single-matched",
                "reference-lds",
                ["PIPELINE=1"],
                0,
                args.repeat,
                False,
            ),
            ("reg", "reference-regtiles", [], 0, args.repeat, False),
            ("naive", "naive", [], 0, 1, False),
        ]
    if args.phase in ("all", "checks"):
        jobs.append(
            (
                "plain-tiled",
                "reference-lds",
                ["PIPELINE=0", "PREFETCH_LDS=0"],
                0,
                1,
                False,
            )
        )
        jobs += [
            (
                f"producer{producers}-p{pipeline}",
                "reference-lds",
                [f"PRODUCERS={producers}", f"PIPELINE={pipeline}"],
                0,
                1,
                False,
            )
            for producers in (64, 128)
            for pipeline in (1, 2)
        ]
        jobs += [
            (f"{name}-seed19", kind, [], 19, 1, False)
            for name, kind in (
                ("lds", "reference-lds"),
                ("reg", "reference-regtiles"),
            )
        ]
    if args.phase in ("all", "traces"):
        jobs += [
            ("trace", "reference-lds", [], 0, 1, True),
            ("producer-trace", "reference-lds", ["PRODUCERS=128"], 0, 1, True),
        ]
    tune = Path(__file__).with_name("tune.py")
    for name, kind, definitions, seed, repeat, trace in jobs:
        command = [
            sys.executable,
            str(tune),
            "delivery-" + name,
            kind,
            "--shape",
            "128",
            "256",
            "128",
            "--seed",
            str(seed),
            "--repeat",
            str(repeat),
        ]
        for definition in definitions:
            command += ["--define", definition]
        if trace:
            command += ["--gem5-arg=--debug-flags=GPUTrace,GPUMem"]
        print("START delivery-" + name, flush=True)
        subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
