#!/usr/bin/env python3
# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause

"""Run the Ruby CHI checkpoint save/restore regression.

For each core count the workload is first run straight through to get the
reference checksum. It is then checkpointed at several points and restored,
and each restored run has to reach the same checksum. A checkpoint whose
cache trace came out empty is rejected: the flush and the replay on restore
would both be no-ops, so the run would pass without exercising anything.
"""

import argparse
import re
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
PAYLOAD_DIR = REPO_ROOT / "tests/test-progs/ruby-checkpoint/src"
CONFIG = REPO_ROOT / "tests/gem5/chi_protocol/configs/chi-checkpoint.py"
CHECKSUM = re.compile(r"^RUBY-CKPT-CHECKSUM ([0-9a-f]+)$", re.MULTILINE)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("gem5", type=Path, help="a RUBY_PROTOCOL_CHI=y binary")
parser.add_argument(
    "--cross-compile",
    default="riscv64-linux-gnu-",
    help="RISC-V GNU toolchain prefix",
)
parser.add_argument(
    "--outdir", type=Path, default=Path("/tmp/ruby-checkpoint-regression")
)
parser.add_argument(
    "--num-cores", type=int, nargs="+", default=[1, 4], help="core counts"
)
parser.add_argument(
    "--checkpoint-at",
    type=int,
    nargs="+",
    default=[200_000_000, 1_000_000_000, 1_800_000_000],
    help="ticks at which to checkpoint; must fall inside the run",
)
parser.add_argument(
    "--timeout-seconds",
    type=int,
    default=600,
    help="per-run limit; a restore that hangs is the failure mode this "
    "regression exists to catch, so it has to be bounded",
)
args = parser.parse_args()


def run(command, name):
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
        # Stranded transactions do not fail, they wait. Say so, rather than
        # letting this surface as a bare traceback.
        raise RuntimeError(
            f"{name} made no progress for {args.timeout_seconds} seconds; "
            "the memory system is most likely deadlocked"
        ) from error
    if result.returncode:
        print(result.stdout, end="")
        raise RuntimeError(f"{name} failed with status {result.returncode}")
    return result.stdout


def checksum_of(output, name):
    match = CHECKSUM.search(output)
    if not match:
        print(output, end="")
        raise RuntimeError(f"{name} printed no checksum")
    return match.group(1)


def gem5_command(outdir, *config_args):
    return [
        args.gem5,
        "--outdir",
        args.outdir / outdir,
        CONFIG,
        payload,
        *config_args,
    ]


payload = PAYLOAD_DIR / "ruby-checkpoint"
run(
    ["make", "-C", PAYLOAD_DIR, f"CROSS_COMPILE={args.cross_compile}"],
    "payload build",
)

for cores in args.num_cores:
    core_args = [f"--num-cores={cores}"]
    reference = checksum_of(
        run(gem5_command(f"reference-{cores}", *core_args), "reference run"),
        "reference run",
    )
    print(f"cores={cores} reference checksum={reference}")

    for tick in args.checkpoint_at:
        label = f"{cores}-{tick}"
        checkpoint = args.outdir / f"checkpoint-{label}"
        save = run(
            gem5_command(
                f"save-{label}",
                *core_args,
                f"--checkpoint-path={checkpoint}",
                f"--take-checkpoint-at={tick}",
            ),
            f"checkpoint at {tick}",
        )
        if "RUBY-CKPT-SAVED" not in save:
            raise RuntimeError(f"no checkpoint written at tick {tick}")

        traces = list(checkpoint.glob("*.cache.gz"))
        if len(traces) != 1:
            raise RuntimeError(f"expected one cache trace, found {traces}")
        # gzip of an empty trace is still ~20 bytes, so compare generously.
        trace_bytes = traces[0].stat().st_size
        if trace_bytes < 1024:
            raise RuntimeError(
                f"cache trace at tick {tick} is {trace_bytes} bytes; the "
                "workload left nothing dirty, so this proves nothing"
            )

        restored = checksum_of(
            run(
                gem5_command(
                    f"restore-{label}",
                    *core_args,
                    f"--checkpoint-path={checkpoint}",
                    "--restore",
                ),
                f"restore from tick {tick}",
            ),
            f"restore from tick {tick}",
        )
        if restored != reference:
            raise RuntimeError(
                f"restored from tick {tick} with {cores} cores gave "
                f"{restored}, expected {reference}"
            )
        print(
            f"  tick={tick} trace={trace_bytes}B checksum={restored} matches"
        )

print("Ruby CHI checkpoint regression passed")
