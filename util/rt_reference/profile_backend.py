# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
"""Collect function-level CPU profiles of the validated eight-core backend."""

import argparse
import hashlib
import json
import os
import subprocess
import time
from pathlib import Path

from validate_backend import (
    ROOT,
    compare,
    inspect,
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--gem5", type=Path, default=ROOT / "build/RISCV/gem5.opt"
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument(
        "--pprof",
        type=Path,
        required=True,
        help="gperftools Perl pprof script",
    )
    parser.add_argument(
        "--profiler",
        type=Path,
        default=Path("/usr/lib/x86_64-linux-gnu/libprofiler.so.0"),
    )
    parser.add_argument("--transport-delay", default="5ns")
    args = parser.parse_args()
    gem5 = args.gem5.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    identity = {
        "gem5_sha256": hashlib.sha256(gem5.read_bytes()).hexdigest(),
        "commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "source_diff_sha256": hashlib.sha256(
            subprocess.check_output(["git", "diff", "HEAD"], cwd=ROOT)
        ).hexdigest(),
        "pprof_sha256": hashlib.sha256(args.pprof.read_bytes()).hexdigest(),
        "profiler_sha256": hashlib.sha256(
            args.profiler.read_bytes()
        ).hexdigest(),
    }
    for queues in (1, 9):
        directory = output / f"q{queues}"
        directory.mkdir(exist_ok=True)
        assert not (directory / "cpu.prof").exists(), directory
        command = [
            str(gem5),
            f"--outdir={directory}",
            str(ROOT / "configs/example/rt_accel.py"),
            "--backend=gem5",
            f"--queues={queues}",
            "--render-mode=1",
            "--samples-per-pixel=1",
            "--max-secondary-depth=0",
            "--seed=1",
            f"--transport-delay={args.transport_delay}",
            "--trace",
        ]
        environment = os.environ.copy()
        environment.update(
            LD_PRELOAD=str(args.profiler.resolve()),
            CPUPROFILE=str(directory / "cpu.prof"),
            CPUPROFILE_FREQUENCY="100",
        )
        print("Profiling:", " ".join(command), flush=True)
        started = time.perf_counter()
        with (directory / "run.log").open("w") as log:
            subprocess.run(
                command,
                cwd=ROOT,
                env=environment,
                stdout=log,
                stderr=subprocess.STDOUT,
                check=True,
            )
        metadata = dict(
            identity,
            command=command,
            wall_seconds=time.perf_counter() - started,
            profiler="gperftools libprofiler, 100 Hz CPU time",
        )
        (directory / "profile.json").write_text(
            json.dumps(metadata, indent=2) + "\n"
        )
        for cumulative in (False, True):
            name = (
                "profile-cumulative.txt" if cumulative else "profile-flat.txt"
            )
            reader = [
                "perl",
                str(args.pprof.resolve()),
                "--text",
                # Use function symbols without expanding the large executable's
                # DWARF inline information. Inline costs remain in their caller.
                "--tools=addr2line:/usr/bin/false,/usr/bin/",
            ]
            if cumulative:
                reader.append("--cum")
            with (directory / name).open("w") as log:
                subprocess.run(
                    reader + [str(gem5), str(directory / "cpu.prof")],
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    check=True,
                )
        inspect(directory, queues)
        compare(args.reference.resolve(), directory)
    print("PASS: profiles preserve output, simulated counters and wire timing")


if __name__ == "__main__":
    main()
