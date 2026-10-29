#!/usr/bin/env python3
"""Build one GEMM variant, inspect its resources, and record a timed KMD run."""

import argparse
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import time
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parents[2]
STUDY = WORKSPACE / "work/gemm-study"
SDK = os.environ.get("HSAKMT_SDK_IMAGE", "gem5-gpu-hsakmt-sdk:migration")
RUNNER = os.environ.get(
    "HSAKMT_RUNNER_IMAGE", "gem5-gpu-hsakmt-runner:lds-final"
)
spec = importlib.util.spec_from_file_location(
    "summary", WORKSPACE / "experiments/eight-cu/summarize.py"
)
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name")
    parser.add_argument(
        "kind",
        choices=[
            "lds",
            "regtiles",
            "naive",
            "mfma-lds",
            "mfma-regtiles",
            "reference-lds",
            "reference-regtiles",
        ],
    )
    parser.add_argument("--define", action="append", default=[])
    parser.add_argument("--shape", nargs=3, type=int, default=[128, 256, 128])
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--build-only", action="store_true")
    parser.add_argument(
        "--repeat",
        type=int,
        default=1,
        help="repeat fresh simulator runs of the same binary",
    )
    parser.add_argument(
        "--gem5-arg",
        action="append",
        default=[],
        help="gem5 debug option; traced wall times are diagnostic",
    )
    parser.add_argument(
        "--config-arg",
        action="append",
        default=[],
        help="option for the gem5 configuration script, e.g. an allocator "
        "policy; changes the modeled platform and is recorded",
    )
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("repeat must be positive")
    if not re.fullmatch(r"[a-z0-9-]+", args.name):
        parser.error("name must contain lowercase letters, digits, or hyphens")
    directory = STUDY / args.name
    directory.mkdir(parents=True, exist_ok=True)
    binary = WORKSPACE / "work/apps/gem5-kmd" / ("gemm-" + args.name)
    source = "hip-matmul" + ("" if args.kind == "naive" else "-" + args.kind)
    source_path = f"/workspace/samples/{source}/main.cpp"
    if args.kind in ("lds", "regtiles"):
        args.define.append("MATMUL_MFMA=0")
    elif args.kind.startswith("reference-"):
        source_path = (
            "/workspace/samples/hip-matmul-"
            + args.kind.removeprefix("reference-")
            + "/main.cpp"
        )
    if args.kind.startswith("mfma-"):
        source_path = "/workspace/experiments/gemm-study/mfma.cpp"
        args.define.append("USE_LDS=" + str(int(args.kind == "mfma-lds")))
    command = [
        "docker",
        "run",
        "--rm",
        "--network",
        "none",
        "--user",
        f"{os.getuid()}:{os.getgid()}",
        "-e",
        "HOME=/tmp",
        "-v",
        f"{WORKSPACE}:/workspace",
        "-w",
        "/workspace/" + str(directory.relative_to(WORKSPACE)),
        SDK,
        "hipcc",
        "--offload-arch=gfx942",
        "-O2",
        "-std=c++17",
        "--save-temps=cwd",
        *["-D" + value for value in args.define],
        source_path,
        "-o",
        "/workspace/" + str(binary.relative_to(WORKSPACE)),
    ]
    record = {
        "name": args.name,
        "kind": args.kind,
        "defines": args.define,
        "shape": args.shape,
        "seed": args.seed,
        "build_command": command,
        "sdk": SDK,
        "runner": RUNNER,
    }
    images = json.loads(
        subprocess.check_output(
            ["docker", "image", "inspect", SDK, RUNNER], text=True
        )
    )
    record["images"] = [
        {key: info[key] for key in ("Id", "Os", "Architecture")}
        for info in images
    ]
    labels = images[1]["Config"].get("Labels") or {}
    record["model_revision"] = labels.get("gpu-lab.model-revision")
    record["model_sha256"] = labels.get("gpu-lab.model-sha256")
    record["gem5_checkout"] = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=WORKSPACE, text=True
    ).strip()
    record["source_sha256"] = {
        str(path.relative_to(WORKSPACE)): hashlib.sha256(
            path.read_bytes()
        ).hexdigest()
        for path in (
            WORKSPACE / source_path.removeprefix("/workspace/"),
            *sorted((WORKSPACE / "samples/common").glob("*.hh")),
        )
    }
    with (directory / "build.log").open("w") as log:
        subprocess.run(
            command, cwd=WORKSPACE, stdout=log, stderr=log, check=True
        )
    record["binary_sha256"] = hashlib.sha256(binary.read_bytes()).hexdigest()
    assembly = next(directory.glob("*gfx942.s")).read_text()
    resources = {}
    for key in (
        "group_segment_fixed_size",
        "private_segment_fixed_size",
        "vgpr_count",
        "sgpr_count",
        "vgpr_spill_count",
        "sgpr_spill_count",
    ):
        resources[key] = [
            int(x) for x in re.findall(r"\." + key + r":\s+(\d+)", assembly)
        ]
    resources["ds_instructions"] = len(re.findall(r"^\s+ds_", assembly, re.M))
    record["resources"] = resources
    # Descriptor reservations can exceed the compiler's used-register metadata.
    for key in ("next_free_vgpr", "next_free_sgpr", "accum_offset"):
        resources[key] = [
            int(x)
            for x in re.findall(r"\.amdhsa_" + key + r"\s+(\d+)", assembly)
        ]
    record["assembly_sha256"] = hashlib.sha256(assembly.encode()).hexdigest()
    if args.kind.endswith("regtiles"):
        assert not any(resources["group_segment_fixed_size"]), resources
        assert resources["ds_instructions"] == 0, resources
    if any(resources["private_segment_fixed_size"]):
        record["skipped"] = "scratch allocation: retain evidence, do not time"
    timed = not args.build_only and "skipped" not in record
    failed = False
    for repetition in range(args.repeat if timed else 1):
        trial = dict(record, repetition=repetition + 1)
        if timed:
            m, n, k = args.shape
            run_id = f"gemm-{args.name}-{m}-{n}-{k}-{time.time_ns()}"
            command = [
                str(WORKSPACE / "scripts/run-app.sh"),
                "--backend",
                "gem5-kmd",
                "--run-name",
                "gemm-" + args.name,
                *["--gem5-arg=" + value for value in args.gem5_arg],
                str(binary.relative_to(WORKSPACE)),
                "--m",
                str(m),
                "--n",
                str(n),
                "--k",
                str(k),
            ]
            if args.seed:
                command += ["--seed", str(args.seed)]
            if args.config_arg:
                command += ["--", *args.config_arg]
            trial["run_command"] = command
            trial["gem5_args"] = args.gem5_arg
            trial["config_args"] = args.config_arg
            environment = dict(
                os.environ,
                HSAKMT_SDK_IMAGE=SDK,
                HSAKMT_RUNNER_IMAGE=RUNNER,
                GPU_LAB_RUN_ID=run_id,
            )
            start = time.monotonic()
            with (directory / f"{run_id}.log").open("w") as log:
                completed = subprocess.run(
                    command,
                    cwd=WORKSPACE,
                    env=environment,
                    stdout=log,
                    stderr=log,
                )
            trial["wall_seconds"] = time.monotonic() - start
            trial["run"] = run_id
            trial["returncode"] = completed.returncode
            if completed.returncode:
                trial["failure"] = (
                    "run failed; excluded from performance comparison"
                )
            else:
                trial.update(
                    summary.summarize(WORKSPACE / "work/runs" / run_id)
                )
        result = directory / f"result-{time.time_ns()}.json"
        result.write_text(json.dumps(trial, indent=2) + "\n")
        print(json.dumps(trial, indent=2), flush=True)
        failed |= "failure" in trial
    if failed:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
