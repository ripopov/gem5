#!/usr/bin/env python3
"""Run and verify the Linux JitCPU-to-C910 UART hello demonstration."""

import argparse
from pathlib import Path
import re
import subprocess
import sys


REPO_ROOT = Path(__file__).resolve().parents[3]
IMAGE_BUILDER = Path(__file__).with_name("build_linux_hello_image.py")
CONFIG = REPO_ROOT / "tests/gem5/jitcpu/configs/jitcpu_linux.py"
HELLO = "Hello world from Linux on the C910 RTL CPU!"


def existing_file(path, description):
    resolved = path.resolve()
    if not resolved.is_file():
        raise FileNotFoundError(f"{description} not found: {resolved}")
    return resolved


def run(command, description, timeout):
    print("+", " ".join(str(argument) for argument in command), flush=True)
    try:
        result = subprocess.run(
            [str(argument) for argument in command],
            cwd=REPO_ROOT,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            f"{description} exceeded {timeout} seconds"
        ) from error
    print(result.stdout, end="")
    if result.returncode:
        raise RuntimeError(
            f"{description} failed with status {result.returncode}"
        )
    return result.stdout


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("gem5", type=Path)
parser.add_argument("backend", type=Path)
parser.add_argument("c910_library", type=Path)
parser.add_argument("base_linux_image", type=Path)
parser.add_argument(
    "--outdir", type=Path, default=Path("/tmp/jit-to-c910-linux-hello")
)
parser.add_argument("--timeout-seconds", type=int, default=3600)
parser.add_argument("--c910-ticks", type=int, default=20_000_000_000)
parser.add_argument("--cc", default="riscv64-linux-gnu-gcc")
args = parser.parse_args()

gem5 = existing_file(args.gem5, "gem5 binary")
backend = existing_file(args.backend, "JitCPU backend")
c910_library = existing_file(args.c910_library, "C910 vendor library")
base_image = existing_file(args.base_linux_image, "base Linux image")
outdir = args.outdir.resolve()
linux_image = outdir / "riscv-jit-c910-linux-hello"
m5out = outdir / "m5out"

run(
    [
        sys.executable,
        IMAGE_BUILDER,
        base_image,
        linux_image,
        "--cc",
        args.cc,
    ],
    "Linux hello image build",
    args.timeout_seconds,
)
output = run(
    [
        gem5,
        "--listener-mode=off",
        "-d",
        m5out,
        CONFIG,
        linux_image,
        backend,
        "--switch-to-c910",
        "--c910-library",
        c910_library,
        "--c910-hello-demo",
        "--c910-ticks",
        str(args.c910_ticks),
    ],
    "Linux JitCPU-to-C910 hello demo",
    args.timeout_seconds,
)

required = (
    "JITCPU_TO_C910_HELLO_PASS",
    "RTL_CPU_HELLO_TIMING ",
)
for marker in required:
    if marker not in output:
        raise RuntimeError(f"demo output omitted marker: {marker}")

terminal = m5out / "system.platform.terminal"
if HELLO not in terminal.read_text(encoding="utf-8"):
    raise RuntimeError("terminal output omitted the C910 hello message")

timing = re.search(
    r"^RTL_CPU_HELLO_TIMING .*boot_host_seconds=([0-9.]+) "
    r".*hello_host_seconds=([0-9.]+)$",
    output,
    re.MULTILINE,
)
if not timing or any(float(value) <= 0 for value in timing.groups()):
    raise RuntimeError("demo reported invalid runtime measurements")

print("JITCPU_TO_C910_HELLO_REGRESSION_PASS")
