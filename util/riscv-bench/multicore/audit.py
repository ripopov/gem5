#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Directed protection, reservation-lifetime and lifecycle regressions."""

import argparse
import configparser
import json
import subprocess
from pathlib import Path

# source, definitions, config arguments, checkpoint save/restore
TESTS = {
    "misaligned-amo": ("alignment.S", ["CASE=2"], [], False),
    "misaligned-amo-crossline": ("alignment.S", ["CASE=4"], [], False),
    "masked-pmp": ("masked-pmp.S", ["CASE=1"], [], False),
    "masked-fault-first": ("masked-pmp.S", ["CASE=2"], [], False),
    "masked-store-pmp": ("masked-pmp.S", ["CASE=3"], [], False),
    "unmasked-fault-first": ("masked-pmp.S", ["CASE=4"], [], False),
    "zero-fault-first": ("masked-pmp.S", ["CASE=5"], [], False),
    "masked-lmul8-load": ("vector-mask.S", ["CASE=1"], [], False),
    "masked-lmul8-store": ("vector-mask.S", ["CASE=2"], [], False),
    "misaligned-lr": ("alignment.S", ["CASE=1"], [], False),
    "misaligned-sc": ("alignment.S", ["CASE=3"], [], False),
    "pmp-partial-s": ("pmp.S", ["CASE=3"], [], False),
    "pmp-partial-m": ("pmp.S", ["CASE=4"], [], False),
    "pmp-revoke-data": ("pmp.S", ["CASE=5"], [], False),
    "pmp-revoke-fetch": ("pmp.S", ["CASE=6"], [], False),
    "sv39-fetch-remap": ("translation.S", ["CASE=1"], [], False),
    "sv39-data-remap": ("translation.S", ["CASE=2"], [], False),
    "reservation-return": (
        "reservation-switch.S",
        [],
        ["--switches", "2"],
        False,
    ),
    "reservation-zero": (
        "reservation-switch.S",
        ["ZERO"],
        ["--lowmem", "--switches", "1"],
        False,
    ),
    "checkpoint-zero": (
        "reservation-switch.S",
        ["ZERO", "CHECKPOINT"],
        ["--lowmem"],
        True,
    ),
    "masked-conflict": ("memory.c", ["CASE=13"], [], False),
    "masked-unmapped": ("memory.c", ["CASE=14"], [], False),
    "word-counter": ("memory.c", ["CASE=15"], [], False),
    "spinlock": ("memory.c", ["CASE=16"], [], False),
    "secondary-ram": ("memory.c", ["CASE=17"], ["--secondary", "ram"], False),
    "secondary-excluded": (
        "memory.c",
        ["CASE=17"],
        ["--secondary", "excluded"],
        False,
    ),
    "checkpoint-reservations": ("memory.c", ["CASE=18"], [], True),
    "software-interrupt": ("interrupt.S", ["CASE=1"], ["--interrupts"], False),
    "timer-interrupt": ("interrupt.S", ["CASE=2"], ["--interrupts"], False),
    "wfi-global-disabled": (
        "interrupt.S",
        ["CASE=3"],
        ["--interrupts"],
        False,
    ),
    "interrupt-after-switch": (
        "interrupt.S",
        ["CASE=1", "SWITCH"],
        ["--interrupts", "--switches", "1"],
        False,
    ),
}

REJECTIONS = {
    "no-ram": "No eligible direct backing store",
    "eventq": "Direct memory must share the CPU's system and event queue",
    "stalls": "Direct memory requires full-system execution without stalls",
}


def main():
    source = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--gem5",
        type=Path,
        default=source.parents[2] / "build/RISCV/gem5.fast",
    )
    parser.add_argument("--outdir", type=Path, required=True)
    parser.add_argument("--cc", default="riscv64-linux-gnu-gcc")
    parser.add_argument("--cores", type=int, choices=(2, 4, 8), default=4)
    parser.add_argument("--channels", type=int, choices=(1, 2, 4), default=4)
    parser.add_argument(
        "--cpu",
        choices=("direct", "noncaching", "atomic", "timing", "minor", "o3"),
        default="direct",
    )
    parser.add_argument(
        "--topology", choices=("flat", "classic", "ruby"), default="flat"
    )
    parser.add_argument("--width", type=int, default=1)
    parser.add_argument("--stagger", action="store_true")
    parser.add_argument("--check-rejections", action="store_true")
    parser.add_argument(
        "--cases", nargs="+", choices=TESTS, default=list(TESTS)
    )
    args = parser.parse_args()
    if args.check_rejections and args.cpu != "direct":
        parser.error("rejection checks require --cpu direct")
    out = args.outdir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    results = []
    for name in args.cases:
        filename, defines, extra, checkpoint = TESTS[name]
        elf = out / f"{name}.elf"
        sources = [source / filename]
        if filename.endswith(".c"):
            sources.insert(0, source / "start.S")
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
            f"-DNHARTS={args.cores}",
            "-DSWITCHES=0",
            *[f"-D{d}" for d in defines],
            f"-Wl,-T,{source / 'link.ld'}",
            "-Wl,--build-id=none",
            *map(str, sources),
            "-o",
            str(elf),
        ]
        subprocess.run(build, check=True, capture_output=True, text=True)
        common = [
            str(source / "config.py"),
            str(elf),
            "--cores",
            str(args.cores),
            "--channels",
            str(args.channels),
            "--cpu",
            args.cpu,
            "--topology",
            args.topology,
            "--width",
            str(args.width),
            *(["--stagger"] if args.stagger else []),
            "--switch-to",
            (
                args.cpu
                if args.cpu in ("direct", "noncaching", "timing")
                else "direct"
            ),
            *extra,
        ]
        phases = [("run", [], "MULTICORE PASS")]
        if checkpoint:
            cpt = out / f"{name}.cpt"
            phases = [
                ("save", ["--checkpoint", str(cpt)], "CHECKPOINT SAVED"),
                ("restore", ["--restore", str(cpt)], "MULTICORE PASS"),
            ]
        for phase, options, marker in phases:
            run = f"{name}-{phase}"
            command = [
                str(args.gem5.resolve()),
                f"--outdir={out / run}",
                *common,
                *options,
            ]
            logfile = out / f"{run}.log"
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
            passed &= marker in logfile.read_text()
            if (
                passed
                and phase == "save"
                and name == "checkpoint-reservations"
                and args.cpu in ("direct", "noncaching")
            ):
                snapshot = configparser.ConfigParser()
                snapshot.read(cpt / "m5.cpt")
                locks = snapshot["system.physmem"]["lal_cid"].split()
                passed = set(map(int, locks)) == set(range(args.cores))
                if not passed:
                    with logfile.open("a") as log:
                        log.write(
                            f"Unexpected checkpoint reservations: {locks}\n"
                        )
            results.append(
                dict(name=run, passed=passed, build=build, command=command)
            )
            (out / "results.json").write_text(
                json.dumps(results, indent=2) + "\n"
            )
            print(f"{'PASS' if passed else 'FAIL'} {run}", flush=True)
            if not passed:
                print(logfile.read_text()[-1500:])
                break
    if args.check_rejections:
        for name, diagnostic in REJECTIONS.items():
            command = [
                str(args.gem5.resolve()),
                f"--outdir={out / ('reject-' + name)}",
                str(source / "config.py"),
                str(elf),
                "--reject",
                name,
            ]
            logfile = out / f"reject-{name}.log"
            with logfile.open("w") as log:
                try:
                    result = subprocess.run(
                        command,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        timeout=60,
                    )
                    passed = result.returncode != 0
                except subprocess.TimeoutExpired:
                    passed = False
            passed &= diagnostic in logfile.read_text()
            results.append(
                dict(name=f"reject-{name}", passed=passed, command=command)
            )
            print(f"{'PASS' if passed else 'FAIL'} reject-{name}", flush=True)
        (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    failed = sum(not r["passed"] for r in results)
    print(f"{len(results) - failed}/{len(results)} passed; logs: {out}")
    return bool(failed)


if __name__ == "__main__":
    raise SystemExit(main())
