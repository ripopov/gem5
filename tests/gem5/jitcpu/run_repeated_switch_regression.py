#!/usr/bin/env python3
"""Run the JitCPU/O3 repeated-switch CHI regression suite."""

import argparse
from pathlib import Path
import subprocess

REPO_ROOT = Path(__file__).resolve().parents[3]
PAYLOAD_DIR = REPO_ROOT / "tests/test-progs/jitcpu-smoke/src"
BAREMETAL_CONFIG = REPO_ROOT / "tests/gem5/jitcpu/configs/jitcpu_baremetal.py"
LINUX_CONFIG = REPO_ROOT / "tests/gem5/jitcpu/configs/jitcpu_linux.py"


def run(command, *, capture=False):
    print("+", " ".join(str(argument) for argument in command), flush=True)
    try:
        return subprocess.run(
            [str(argument) for argument in command],
            cwd=REPO_ROOT,
            check=False,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.STDOUT if capture else None,
            text=True,
            timeout=args.timeout_seconds,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            f"command exceeded {args.timeout_seconds}-second timeout"
        ) from error


def require_success(command, description):
    result = run(command)
    if result.returncode:
        raise RuntimeError(
            f"{description} failed with status {result.returncode}"
        )


parser = argparse.ArgumentParser()
parser.add_argument("gem5", type=Path)
parser.add_argument("backend", type=Path)
parser.add_argument(
    "--cross-compile",
    default="riscv64-linux-gnu-",
    help="RISC-V GNU toolchain prefix",
)
parser.add_argument(
    "--outdir",
    type=Path,
    default=Path("/tmp/jitcpu-repeated-switch-regression"),
)
parser.add_argument(
    "--linux-image",
    type=Path,
    help="also run the Linux repeated-switch stress using this fixed image",
)
parser.add_argument("--linux-switches", type=int, default=11)
parser.add_argument("--smp-linux-switches", type=int, default=21)
parser.add_argument(
    "--smp-garnet",
    action="store_true",
    help="repeat the four-core directed and Linux stresses with Garnet",
)
parser.add_argument("--timeout-seconds", type=int, default=900)
args = parser.parse_args()

gem5 = args.gem5.resolve()
backend = args.backend.resolve()
for artifact in (gem5, backend):
    if not artifact.is_file():
        parser.error(f"file does not exist: {artifact}")

require_success(
    [
        "make",
        "-C",
        PAYLOAD_DIR,
        f"CROSS_COMPILE={args.cross_compile}",
        "jitcpu-repeated-switch",
        "jitcpu-smp-repeated-switch",
        "jitcpu-smp-wfi-repeated-switch",
        "jitcpu-linux-init.cpio",
        "jitcpu-linux-smp-init.cpio",
    ],
    "payload build",
)

baremetal = PAYLOAD_DIR / "jitcpu-repeated-switch"
baremetal_smp = PAYLOAD_DIR / "jitcpu-smp-repeated-switch"
baremetal_smp_wfi = PAYLOAD_DIR / "jitcpu-smp-wfi-repeated-switch"
initrd = PAYLOAD_DIR / "jitcpu-linux-init.cpio"
smp_initrd = PAYLOAD_DIR / "jitcpu-linux-smp-init.cpio"
require_success(
    [
        gem5,
        f"--outdir={args.outdir / 'baremetal-positive'}",
        BAREMETAL_CONFIG,
        baremetal,
        backend,
        "--ruby-chi",
        "--repeated-switches",
        "21",
        "--max-ticks",
        "100000000",
    ],
    "21-switch bare-metal regression",
)

negative = run(
    [
        gem5,
        f"--outdir={args.outdir / 'baremetal-negative'}",
        BAREMETAL_CONFIG,
        baremetal,
        backend,
        "--ruby-chi",
        "--repeated-switches",
        "2",
        "--max-ticks",
        "100000000",
        "--unsafe-skip-ruby-maintenance",
    ],
    capture=True,
)
print(negative.stdout, end="")
negative_proof = "m5_fail instruction encountered (code 9)"
if negative.returncode == 0 or negative_proof not in negative.stdout:
    raise RuntimeError(
        "negative control did not expose stale cached phase state as "
        f"expected (status {negative.returncode})"
    )
print("Negative control failed with stale cached data as expected")


def run_smp_baremetal(network, payload, variant):
    command = [
        gem5,
        f"--outdir={args.outdir / f'baremetal-smp-{variant}-{network}'}",
        BAREMETAL_CONFIG,
        payload,
        backend,
        "--ruby-chi",
        "--num-cpus",
        "4",
        "--num-dirs",
        "2",
        "--num-l3caches",
        "4",
        "--repeated-switches",
        "21",
        "--max-ticks",
        "200000000",
    ]
    if network == "garnet":
        command.extend(["--ruby-network", "garnet"])
    require_success(
        command,
        f"four-core/two-controller {network} {variant} regression",
    )


run_smp_baremetal("simple", baremetal_smp, "active")
run_smp_baremetal("simple", baremetal_smp_wfi, "wfi")
if args.smp_garnet:
    run_smp_baremetal("garnet", baremetal_smp, "active")
    run_smp_baremetal("garnet", baremetal_smp_wfi, "wfi")

if args.linux_image:
    linux_image = args.linux_image.resolve()
    if not linux_image.is_file():
        parser.error(f"Linux image does not exist: {linux_image}")
    if args.linux_switches < 3 or args.linux_switches % 2 == 0:
        parser.error("--linux-switches must be an odd value of at least 3")
    if args.smp_linux_switches < 3 or args.smp_linux_switches % 2 == 0:
        parser.error("--smp-linux-switches must be an odd value of at least 3")

    require_success(
        [
            gem5,
            f"--outdir={args.outdir / 'linux'}",
            LINUX_CONFIG,
            linux_image,
            backend,
            "--ruby-chi",
            "--repeated-switches",
            str(args.linux_switches),
            "--phase-ticks",
            "100000000",
            "--final-o3-ticks",
            "1000000000",
            "--initrd",
            initrd,
            "--max-ticks",
            "2000000000000",
        ],
        "Linux repeated-switch stress",
    )

    def run_smp_linux(network):
        command = [
            gem5,
            f"--outdir={args.outdir / f'linux-smp-{network}'}",
            LINUX_CONFIG,
            linux_image,
            backend,
            "--ruby-chi",
            "--num-cpus",
            "4",
            "--num-dirs",
            "2",
            "--num-l3caches",
            "4",
            "--repeated-switches",
            str(args.smp_linux_switches),
            "--phase-ticks",
            "100000000",
            "--final-o3-ticks",
            "1000000000",
            "--initrd",
            smp_initrd,
            "--max-ticks",
            "2000000000000",
        ]
        if network == "garnet":
            command.extend(["--ruby-network", "garnet"])
        require_success(
            command,
            f"four-core/two-controller {network} Linux stress",
        )

    run_smp_linux("simple")
    if args.smp_garnet:
        run_smp_linux("garnet")
else:
    print("Linux stress skipped (pass --linux-image to enable it)")

print("JitCPU repeated-switch regression suite passed")
