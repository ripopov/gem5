#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Build and run deterministic bare-metal multicore memory regressions."""

import argparse
import itertools
import json
import subprocess
from pathlib import Path

CASES = {
    "reservation-store": 1,
    "reservation-amo": 2,
    "reservation-vector": 3,
    "reservation-zero": 4,
    "lrsc-counter": 5,
    "mixed-counter": 6,
    "producer-consumer": 7,
    "reservation-unaligned": 8,
    "reservation-reuse": 9,
    "reservation-compete": 10,
    "readonly-amo": 11,
    "instruction-publish": 12,
}


def main():
    source = Path(__file__).resolve().parent
    root = source.parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--gem5", type=Path, default=root / "build/RISCV/gem5.fast"
    )
    parser.add_argument("--cc", default="riscv64-linux-gnu-gcc")
    parser.add_argument("--outdir", type=Path, required=True)
    parser.add_argument(
        "--cores", type=int, nargs="+", choices=(2, 4, 8), default=[2, 4]
    )
    parser.add_argument(
        "--channels", type=int, nargs="+", choices=(1, 2, 4), default=[1, 4]
    )
    parser.add_argument(
        "--topology", choices=("flat", "classic", "ruby"), default="flat"
    )
    parser.add_argument(
        "--memory", choices=("simple", "ddr4"), default="simple"
    )
    parser.add_argument(
        "--cpu", choices=("direct", "noncaching", "mixed"), default="direct"
    )
    parser.add_argument("--switches", type=int, default=0)
    parser.add_argument(
        "--switch-to",
        choices=("direct", "noncaching", "timing"),
        default="direct",
    )
    parser.add_argument(
        "--cases", nargs="+", choices=CASES, default=list(CASES)
    )
    args = parser.parse_args()
    if args.switches < 0:
        parser.error("--switches must be nonnegative")
    out = args.outdir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    results = []
    for cores, case in itertools.product(args.cores, args.cases):
        elf = out / f"{case}-{cores}.elf"
        build = [
            args.cc,
            "-O2",
            "-ffreestanding",
            "-nostdlib",
            "-static",
            "-fno-pie",
            "-no-pie",
            "-fno-builtin",
            "-mcmodel=medany",
            "-msmall-data-limit=0",
            "-mno-relax",
            "-march=rv64gcv_zicboz",
            "-mabi=lp64d",
            f"-DNHARTS={cores}",
            f"-DCASE={CASES[case]}",
            f"-DSWITCHES={args.switches}",
            f"-Wl,-T,{source / 'link.ld'}",
            "-Wl,--build-id=none",
            str(source / "start.S"),
            str(source / "memory.c"),
            "-o",
            str(elf),
        ]
        subprocess.run(build, check=True, capture_output=True, text=True)
        for channels in args.channels:
            name = f"{case}-{cores}h-{channels}ch"
            command = [
                str(args.gem5.resolve()),
                f"--outdir={out / name}",
                str(source / "config.py"),
                str(elf),
                "--cores",
                str(cores),
                "--channels",
                str(channels),
                "--cpu",
                args.cpu,
                "--topology",
                args.topology,
                "--memory",
                args.memory,
                "--switches",
                str(args.switches),
                "--switch-to",
                args.switch_to,
            ]
            logfile = out / f"{name}.log"
            with logfile.open("w") as log:
                try:
                    result = subprocess.run(
                        command,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        timeout=60,
                    )
                    passed = result.returncode == 0
                except subprocess.TimeoutExpired:
                    passed = False
            passed &= "MULTICORE PASS" in logfile.read_text()
            results.append(
                dict(name=name, passed=passed, command=command, build=build)
            )
            (out / "results.json").write_text(
                json.dumps(results, indent=2) + "\n"
            )
            print(f"{'PASS' if passed else 'FAIL'} {name}", flush=True)
            if not passed:
                print(logfile.read_text()[-3000:])
    failed = sum(not r["passed"] for r in results)
    print(f"{len(results) - failed}/{len(results)} passed; logs: {out}")
    return bool(failed)


if __name__ == "__main__":
    raise SystemExit(main())
