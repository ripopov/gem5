#!/usr/bin/env python3
# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause

"""Run the 16-core JitCPU/CHI mesh and Linux handoff regressions."""

import argparse
import re
import subprocess
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
PAYLOAD_DIR = REPO_ROOT / "tests/test-progs/jitcpu-smoke/src"
BAREMETAL_CONFIG = REPO_ROOT / "tests/gem5/jitcpu/configs/jitcpu_baremetal.py"
LINUX_CONFIG = REPO_ROOT / "tests/gem5/jitcpu/configs/jitcpu_linux.py"
EXISTING_DRIVER = (
    REPO_ROOT / "tests/gem5/jitcpu/run_repeated_switch_regression.py"
)


parser = argparse.ArgumentParser()
parser.add_argument("gem5", type=Path)
parser.add_argument("backend", type=Path)
parser.add_argument(
    "linux_image",
    type=Path,
    help="OpenSBI bootloader ELF used for the 16-hart Linux runs",
)
parser.add_argument(
    "--kernel",
    type=Path,
    required=True,
    help="16-hart-capable RISC-V Linux ELF",
)
parser.add_argument(
    "--cross-compile",
    default="riscv64-linux-gnu-",
    help="RISC-V GNU toolchain prefix",
)
parser.add_argument(
    "--musl-cc",
    default="riscv64-linux-musl-gcc",
    help="RISC-V musl compiler used for the static pthread workload",
)
parser.add_argument(
    "--outdir",
    type=Path,
    default=Path("/tmp/jitcpu-16core-mesh-regression"),
)
parser.add_argument("--linux-runs", type=int, default=3)
parser.add_argument(
    "--workload-switches",
    type=int,
    default=5,
    help=(
        "odd number of userspace-coordinated switches in the 16-core "
        "repeated Linux run"
    ),
)
parser.add_argument("--timeout-seconds", type=int, default=3600)
parser.add_argument(
    "--skip-existing-regressions",
    action="store_true",
    help="skip the final focused FLUSH and 1/4-core SimpleNetwork suite",
)
args = parser.parse_args()

if args.linux_runs < 2:
    parser.error("--linux-runs must be at least 2")
if args.workload_switches < 3 or args.workload_switches % 2 == 0:
    parser.error("--workload-switches must be an odd value of at least 3")

gem5 = args.gem5.resolve()
backend = args.backend.resolve()
linux_image = args.linux_image.resolve()
kernel = args.kernel.resolve()
# The payload build runs make -C, so a relative compiler path would be
# resolved against the payload directory instead of the caller's cwd.
musl_cc = args.musl_cc
if "/" in musl_cc:
    musl_cc = str(Path(musl_cc).resolve())
for artifact in (gem5, backend, linux_image, kernel):
    if not artifact.is_file():
        parser.error(f"file does not exist: {artifact}")


def run(command, description, timeout=None):
    rendered = [str(argument) for argument in command]
    print("+", " ".join(rendered), flush=True)
    started = time.monotonic()
    try:
        result = subprocess.run(
            rendered,
            cwd=REPO_ROOT,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout or args.timeout_seconds,
        )
    except subprocess.TimeoutExpired as error:
        if error.stdout:
            print(error.stdout, end="")
        raise RuntimeError(
            f"{description} exceeded its host timeout"
        ) from error
    elapsed = time.monotonic() - started
    print(result.stdout, end="")
    if result.returncode:
        raise RuntimeError(
            f"{description} failed with status {result.returncode}"
        )
    print(f"{description} passed in {elapsed:.2f} host seconds", flush=True)
    return result.stdout, elapsed


def read_terminal(outdir):
    path = outdir / "system.platform.terminal"
    if not path.is_file():
        raise RuntimeError(f"guest terminal log is missing: {path}")
    return path.read_text(encoding="utf-8", errors="replace")


def require_markers(guest_log, markers, description):
    missing = [marker for marker in markers if marker not in guest_log]
    if missing:
        raise RuntimeError(
            f"{description} is missing marker(s): {', '.join(missing)}"
        )


def require_no_vector_instructions(binary):
    command = [f"{args.cross_compile}objdump", "-d", str(binary)]
    result = subprocess.run(
        command,
        cwd=REPO_ROOT,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=args.timeout_seconds,
    )
    if result.returncode:
        raise RuntimeError(
            f"failed to disassemble dining workload with {command[0]}"
        )
    vector_instructions = []
    for line in result.stdout.splitlines():
        match = re.match(r"^\s*[0-9a-f]+:\s+[0-9a-f]+\s+(\S+)", line)
        if match and match.group(1).startswith("v"):
            vector_instructions.append(line.strip())
    if vector_instructions:
        examples = "; ".join(vector_instructions[:5])
        raise RuntimeError(
            "dining workload contains unsupported vector instructions: "
            + examples
        )
    print("Dining workload RV64 scalar-ISA check passed", flush=True)


run(
    [
        "make",
        "-C",
        PAYLOAD_DIR,
        f"CROSS_COMPILE={args.cross_compile}",
        f"DINING_CC={musl_cc}",
        "jitcpu-16core-mesh-switch",
        "jitcpu-dining-philosophers.cpio",
    ],
    "16-core payload build",
)
run(
    [
        "make",
        "-B",
        "-C",
        PAYLOAD_DIR,
        f"DINING_CC={musl_cc}",
        f"DINING_SWITCHES={args.workload_switches}",
        "jitcpu-dining-philosophers-repeated.cpio",
    ],
    "16-core repeated payload build",
)
require_no_vector_instructions(PAYLOAD_DIR / "jitcpu-dining-philosophers")
require_no_vector_instructions(
    PAYLOAD_DIR / "jitcpu-dining-philosophers-repeated"
)

baremetal_outdir = args.outdir / "baremetal"
baremetal_output, baremetal_host_time = run(
    [
        gem5,
        f"--outdir={baremetal_outdir}",
        BAREMETAL_CONFIG,
        PAYLOAD_DIR / "jitcpu-16core-mesh-switch",
        backend,
        "--chi-4x4-mesh",
        "--switch-to-o3",
        "--max-ticks",
        "2000000000",
    ],
    "16-core bare-metal SimpleNetwork mesh handoff",
)
require_markers(
    read_terminal(baremetal_outdir),
    ("JITCPU-MESH16 READY", "JITCPU-MESH16 PASS"),
    "bare-metal guest log",
)
require_markers(
    baremetal_output,
    (
        "Validated CHI SimpleNetwork 4x4 XY mesh",
        "cpu generated 0 timing CHI messages",
        "o3 per-RNF CHI cache accesses:",
        "o3 per-controller memory bytes:",
    ),
    "bare-metal host validation",
)

linux_host_times = []
linux_ticks = []
for run_index in range(args.linux_runs):
    linux_outdir = args.outdir / f"linux-{run_index + 1}"
    output, elapsed = run(
        [
            gem5,
            f"--outdir={linux_outdir}",
            LINUX_CONFIG,
            linux_image,
            backend,
            "--kernel",
            kernel,
            "--chi-4x4-mesh",
            "--workload-handoff",
            "--initrd",
            PAYLOAD_DIR / "jitcpu-dining-philosophers.cpio",
            "--max-ticks",
            "2000000000000",
            "--o3-ticks",
            "50000000000",
        ],
        f"16-core Linux dining-philosophers run {run_index + 1}",
    )
    linux_host_times.append(elapsed)
    matches = re.findall(
        r"16-core Linux dining-philosophers handoff passed on O3 in "
        r"timing mode @ tick (\d+)",
        output,
    )
    if len(matches) != 1:
        raise RuntimeError(
            f"Linux run {run_index + 1} did not report exactly one final tick"
        )
    linux_ticks.append(int(matches[0]))
    guest_log = read_terminal(linux_outdir)
    require_markers(
        guest_log,
        (
            "JITCPU-DINING ONLINE cpus=16",
            "JITCPU-DINING READY",
            "JITCPU-DINING PRE-SWITCH-PASS",
            "JITCPU-DINING POST-SWITCH-PROGRESS",
            "JITCPU-DINING PASS",
        ),
        f"Linux run {run_index + 1} guest log",
    )

if len(set(linux_ticks)) != 1:
    raise RuntimeError(f"Linux runs ended at different ticks: {linux_ticks}")

repeated_outdir = args.outdir / "linux-repeated"
repeated_output, repeated_host_time = run(
    [
        gem5,
        f"--outdir={repeated_outdir}",
        LINUX_CONFIG,
        linux_image,
        backend,
        "--kernel",
        kernel,
        "--chi-4x4-mesh",
        "--workload-switches",
        str(args.workload_switches),
        "--initrd",
        PAYLOAD_DIR / "jitcpu-dining-philosophers-repeated.cpio",
        "--max-ticks",
        "2000000000000",
        "--o3-ticks",
        "50000000000",
    ],
    (
        "16-core Linux dining-philosophers "
        f"{args.workload_switches}-switch run"
    ),
)
repeated_matches = re.findall(
    r"16-core Linux dining-philosophers repeated-switch validation "
    rf"passed after {args.workload_switches} switches @ tick (\d+)",
    repeated_output,
)
if len(repeated_matches) != 1:
    raise RuntimeError(
        "16-core repeated Linux run did not report exactly one final tick"
    )
repeated_tick = int(repeated_matches[0])
repeated_guest_log = read_terminal(repeated_outdir)
required_repeated_markers = [
    "JITCPU-DINING ONLINE cpus=16",
    "JITCPU-DINING READY",
    (f"JITCPU-DINING PASS workers=16 " f"switches={args.workload_switches}"),
]
for phase in range(args.workload_switches + 1):
    model = "jit" if phase % 2 == 0 else "o3"
    required_repeated_markers.append(
        f"JITCPU-DINING PHASE-PASS phase={phase} model={model}"
    )
for switch_index in range(1, args.workload_switches + 1):
    source = "jit" if switch_index % 2 == 1 else "o3"
    destination = "o3" if source == "jit" else "jit"
    required_repeated_markers.append(
        f"JITCPU-DINING SWITCH-REQUEST index={switch_index} "
        f"from={source} to={destination}"
    )
require_markers(
    repeated_guest_log,
    required_repeated_markers,
    "16-core repeated Linux guest log",
)

if not args.skip_existing_regressions:
    run(
        [
            "python3",
            EXISTING_DRIVER,
            gem5,
            backend,
            "--cross-compile",
            args.cross_compile,
            "--outdir",
            args.outdir / "existing",
            "--linux-image",
            linux_image,
            "--linux-kernel",
            kernel,
            "--timeout-seconds",
            str(args.timeout_seconds),
        ],
        "focused FLUSH and existing 1/4-core SimpleNetwork regressions",
        timeout=args.timeout_seconds * 8,
    )

print(
    "16-core mesh regression suite passed: "
    f"bare-metal host_time={baremetal_host_time:.2f}s, "
    f"linux_ticks={linux_ticks}, "
    "linux_host_times="
    + ",".join(f"{value:.2f}s" for value in linux_host_times)
    + f", repeated_switches={args.workload_switches}, "
    f"repeated_tick={repeated_tick}, "
    f"repeated_host_time={repeated_host_time:.2f}s"
)
