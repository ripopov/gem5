#!/usr/bin/env python3
"""Run the complete one-way JitCPU-to-C910 takeover regression."""

import argparse
from pathlib import Path
import subprocess
from typing import (
    List,
    Union,
)


REPO_ROOT = Path(__file__).resolve().parents[3]
SWITCH_CONFIG = (
    REPO_ROOT / "configs/example/rtl_cosim/c910_cpu_switch.py"
)
LINUX_CONFIG = REPO_ROOT / "tests/gem5/jitcpu/configs/jitcpu_linux.py"
CASES = (
    "integer",
    "compute",
    "fp",
    "trap",
    "sv39",
    "user",
    "atomic",
    "lrsc",
    "interrupt",
    "external_interrupt",
)


def run(command: List[Union[Path, str]], description: str) -> str:
    print("+", " ".join(str(argument) for argument in command), flush=True)
    try:
        result = subprocess.run(
            [str(argument) for argument in command],
            cwd=REPO_ROOT,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=args.timeout_seconds,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            f"{description} exceeded {args.timeout_seconds} seconds"
        ) from error
    print(result.stdout, end="")
    if result.returncode:
        raise RuntimeError(
            f"{description} failed with status {result.returncode}"
        )
    return result.stdout


def existing_file(path: Path, description: str) -> Path:
    path = path.resolve()
    if not path.is_file():
        raise FileNotFoundError(f"{description} not found: {path}")
    return path


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("gem5", type=Path)
parser.add_argument("backend", type=Path)
parser.add_argument("c910_library", type=Path)
parser.add_argument("program_dir", type=Path)
parser.add_argument("linux_image", type=Path)
parser.add_argument(
    "--outdir", type=Path, default=Path("/tmp/jit-to-c910-regression")
)
parser.add_argument("--timeout-seconds", type=int, default=900)
parser.add_argument("--c910-linux-ticks", type=int, default=100_000_000)
args = parser.parse_args()

gem5 = existing_file(args.gem5, "gem5 binary")
backend = existing_file(args.backend, "JitCPU backend")
c910_library = existing_file(args.c910_library, "C910 vendor library")
linux_image = existing_file(args.linux_image, "Linux image")
program_dir = args.program_dir.resolve()

for case in CASES:
    image = existing_file(
        program_dir / f"switch_{case}.elf", f"{case} bare-metal image"
    )
    output = run(
        [
            gem5,
            "-d",
            args.outdir / f"baremetal-{case}",
            SWITCH_CONFIG,
            "--library",
            c910_library,
            "--image",
            image,
            "--source-cpu",
            "jit",
            "--jit-backend",
            backend,
            "--case",
            case,
        ],
        f"bare-metal {case} takeover",
    )
    markers = (
        "RTL_COSIM_CPU_SWITCH_ONE_WAY_PASS",
        f"RTL_COSIM_JIT_TO_C910_PASS case={case}",
    )
    for marker in markers:
        if marker not in output:
            raise RuntimeError(
                f"bare-metal {case} takeover omitted marker: {marker}"
            )

linux_output = run(
    [
        gem5,
        "-d",
        args.outdir / "linux",
        LINUX_CONFIG,
        linux_image,
        backend,
        "--switch-to-c910",
        "--c910-library",
        c910_library,
        "--c910-ticks",
        str(args.c910_linux_ticks),
    ],
    "Linux takeover",
)
if "JITCPU_TO_C910_LINUX_PASS" not in linux_output:
    raise RuntimeError("Linux takeover omitted its pass marker")

print("JITCPU_TO_C910_REGRESSION_PASS")
