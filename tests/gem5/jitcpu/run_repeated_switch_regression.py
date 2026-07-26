#!/usr/bin/env python3
"""Run the JitCPU/O3 repeated-switch CHI regression suite."""

import argparse
import subprocess
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
PAYLOAD_DIR = REPO_ROOT / "tests/test-progs/jitcpu-smoke/src"
BAREMETAL_CONFIG = REPO_ROOT / "tests/gem5/jitcpu/configs/jitcpu_baremetal.py"
LINUX_CONFIG = REPO_ROOT / "tests/gem5/jitcpu/configs/jitcpu_linux.py"
RUBY_RANDOM_CONFIG = REPO_ROOT / "configs/example/ruby_random_test.py"


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
parser.add_argument(
    "--linux-kernel",
    type=Path,
    help=(
        "optional separate Linux ELF; when set, --linux-image is treated "
        "as the OpenSBI bootloader"
    ),
)
parser.add_argument("--linux-switches", type=int, default=11)
parser.add_argument("--smp-linux-switches", type=int, default=21)
parser.add_argument("--timeout-seconds", type=int, default=900)
args = parser.parse_args()

gem5 = args.gem5.resolve()
backend = args.backend.resolve()
for artifact in (gem5, backend):
    if not artifact.is_file():
        parser.error(f"file does not exist: {artifact}")


def run_chi_flush(num_cpus, num_dirs, num_l3caches):
    require_success(
        [
            gem5,
            f"--outdir={args.outdir / f'chi-flush-{num_cpus}x{num_dirs}'}",
            RUBY_RANDOM_CONFIG,
            f"--num-cpus={num_cpus}",
            f"--num-dirs={num_dirs}",
            f"--num-l3caches={num_l3caches}",
            "--maxloads=5000",
            "--abs-max-tick=10000000000",
            "--network=simple",
            "--check-flush",
            "--flush-period=127",
            "--flush-duplicates",
        ],
        f"{num_cpus}-requester/{num_dirs}-controller CHI FLUSH regression",
    )


run_chi_flush(4, 2, 4)
run_chi_flush(8, 4, 8)

require_success(
    [
        "make",
        "-C",
        PAYLOAD_DIR,
        f"CROSS_COMPILE={args.cross_compile}",
        "jitcpu-repeated-switch",
        "jitcpu-smp-repeated-switch",
        "jitcpu-smp-wfi-repeated-switch",
        "jitcpu-smp8-repeated-switch",
        "jitcpu-mesh16-repeated-switch",
        "jitcpu-mesh16-wfi-repeated-switch",
        "jitcpu-linux-init.cpio",
        "jitcpu-linux-smp-init.cpio",
    ],
    "payload build",
)

baremetal = PAYLOAD_DIR / "jitcpu-repeated-switch"
baremetal_smp = PAYLOAD_DIR / "jitcpu-smp-repeated-switch"
baremetal_smp_wfi = PAYLOAD_DIR / "jitcpu-smp-wfi-repeated-switch"
baremetal_smp8 = PAYLOAD_DIR / "jitcpu-smp8-repeated-switch"
baremetal_mesh16 = PAYLOAD_DIR / "jitcpu-mesh16-repeated-switch"
baremetal_mesh16_wfi = PAYLOAD_DIR / "jitcpu-mesh16-wfi-repeated-switch"
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


def run_smp_baremetal(
    payload, variant, num_cpus, num_dirs, num_l3caches, extra=()
):
    require_success(
        [
            gem5,
            f"--outdir={args.outdir / f'baremetal-smp{num_cpus}-{variant}'}",
            BAREMETAL_CONFIG,
            payload,
            backend,
            "--ruby-chi",
            "--num-cpus",
            str(num_cpus),
            "--num-dirs",
            str(num_dirs),
            "--num-l3caches",
            str(num_l3caches),
            "--repeated-switches",
            "21",
            "--max-ticks",
            # Trace-driven FLUSH intentionally serializes one hierarchy-wide
            # transaction per recorded cache block. The multi-core footprint
            # can therefore consume more than the old 200M-tick phase guard.
            "1000000000",
            *extra,
        ],
        f"{num_cpus}-core/{num_dirs}-controller {variant} regression",
    )


def run_mesh16_baremetal(payload, variant):
    require_success(
        [
            gem5,
            f"--outdir={args.outdir / f'baremetal-mesh16-{variant}'}",
            BAREMETAL_CONFIG,
            payload,
            backend,
            "--chi-4x4-mesh",
            "--repeated-switches",
            "21",
            "--max-ticks",
            "4000000000",
        ],
        f"16-core 4x4 mesh {variant} repeated-switch regression",
    )


run_smp_baremetal(baremetal_smp, "active", 4, 2, 4)
run_smp_baremetal(baremetal_smp_wfi, "wfi", 4, 2, 4)
run_smp_baremetal(baremetal_smp8, "active", 8, 4, 8)

# Batch length decides how far one hart runs ahead of its peers between event
# boundaries. The default platform RTC caps every batch at ten instructions,
# so cover both remaining regimes explicitly: single-instruction stepping, and
# long batches that let a hart race far ahead before the queue is serviced.
run_smp_baremetal(baremetal_smp, "step", 4, 2, 4, extra=("--batch-size", "1"))
run_smp_baremetal(
    baremetal_smp,
    "longbatch",
    4,
    2,
    4,
    extra=("--batch-size", "8192", "--rtc-frequency", "100kHz"),
)
run_smp_baremetal(
    baremetal_smp_wfi,
    "wfi-longbatch",
    4,
    2,
    4,
    extra=("--batch-size", "8192", "--rtc-frequency", "100kHz"),
)
run_mesh16_baremetal(baremetal_mesh16, "active")
run_mesh16_baremetal(baremetal_mesh16_wfi, "wfi")

if args.linux_image:
    linux_image = args.linux_image.resolve()
    if not linux_image.is_file():
        parser.error(f"Linux image does not exist: {linux_image}")
    linux_kernel = args.linux_kernel.resolve() if args.linux_kernel else None
    if linux_kernel and not linux_kernel.is_file():
        parser.error(f"Linux kernel does not exist: {linux_kernel}")
    if args.linux_switches < 3 or args.linux_switches % 2 == 0:
        parser.error("--linux-switches must be an odd value of at least 3")
    if args.smp_linux_switches < 3 or args.smp_linux_switches % 2 == 0:
        parser.error("--smp-linux-switches must be an odd value of at least 3")

    linux_command = [
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
    ]
    if linux_kernel:
        linux_command.extend(["--kernel", linux_kernel])
    require_success(
        linux_command,
        "Linux repeated-switch stress",
    )

    smp_linux_command = [
        gem5,
        f"--outdir={args.outdir / 'linux-smp'}",
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
    if linux_kernel:
        smp_linux_command.extend(["--kernel", linux_kernel])
    require_success(
        smp_linux_command,
        "four-core/two-controller Linux stress",
    )
else:
    print("Linux stress skipped (pass --linux-image to enable it)")

print("JitCPU repeated-switch regression suite passed")
