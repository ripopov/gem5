#!/usr/bin/env python3
"""Collect completed tuning results and the counters used to interpret them."""

import argparse
import csv
import json
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parents[2]


def counters(run):
    stats = {}
    for line in (run / "stats.txt").read_text().splitlines():
        fields = line.split()
        if len(fields) > 1:
            try:
                stats[fields[0]] = float(fields[1])
            except ValueError:
                pass
    result = {}
    names = [
        "vALUInsts",
        "sALUInsts",
        "globalReads",
        "globalWrites",
        "groupReads",
        "groupWrites",
        "ldsBankAccesses",
        "spillReads",
        "spillWrites",
        "numVecOpsExecutedMFMA",
    ]
    for name in names:
        result[name] = sum(
            value
            for key, value in stats.items()
            if key.startswith("system.cpu1.CUs") and key.endswith("." + name)
        )
    for reason in (
        "IBEmpty",
        "WaitCnt",
        "BarrierWait",
        "VgprBusy",
        "SgprBusy",
        "MatrixCore",
        "InstrReady",
    ):
        result["wave_slot_cycles_" + reason] = sum(
            value
            for key, value in stats.items()
            if key.startswith("system.cpu1.CUs")
            and key.endswith("stallCycles::" + reason)
        )
    result["extra_bank_serialization"] = sum(
        value * stats[key.replace("::mean", "::samples")]
        for key, value in stats.items()
        if key.startswith("system.cpu1.CUs")
        and key.endswith("ldsBankConflictDist::mean")
        and stats[key.replace("::mean", "::samples")] > 0
    )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--csv", type=Path, help="compact exploration summary")
    parser.add_argument("--prefix", default="", help="select experiment names")
    args = parser.parse_args()
    records = []
    for path in sorted(
        (WORKSPACE / "work/gemm-study").glob("*/result-*.json")
    ):
        record = json.loads(path.read_text())
        if not record["name"].startswith(args.prefix):
            continue
        if args.output and record.get("gpu_active_cycles") is not None:
            record["counters"] = counters(
                WORKSPACE / "work/runs" / record["run"]
            )
        records.append(record)
    if args.output:
        args.output.write_text(
            json.dumps(records, indent=2, allow_nan=False) + "\n"
        )
    if args.csv:
        fields = [
            "name",
            "kind",
            "shape",
            "defines",
            "seed",
            "status",
            "run",
            "gpu_active_cycles",
            "wall_seconds",
            "host_seconds",
            "runner",
            "model_image",
            "binary_sha256",
            "assembly_sha256",
        ]
        with args.csv.open("w", newline="") as output:
            writer = csv.DictWriter(
                output, fieldnames=fields, lineterminator="\n"
            )
            writer.writeheader()
            for record in records:
                row = {key: record.get(key, "") for key in fields}
                row["shape"] = "x".join(map(str, record["shape"]))
                row["defines"] = ";".join(record["defines"])
                row["status"] = (
                    record.get("failure")
                    or record.get("skipped")
                    or (
                        "pass"
                        if record.get("gpu_active_cycles") is not None
                        else "build only"
                    )
                )
                if record.get("gem5_args"):
                    row["status"] = "diagnostic trace"
                if record.get("images"):
                    row["model_image"] = record["images"][1]["Id"]
                writer.writerow(row)
    if not args.output and not args.csv:
        for r in records:
            cycles = r.get("gpu_active_cycles")
            print(
                r["name"],
                "x".join(map(str, r["shape"])),
                (
                    round(cycles)
                    if cycles
                    else r.get("failure", r.get("skipped", "build only"))
                ),
                "cycles",
                round(r.get("wall_seconds", 0), 2),
                "s",
            )


if __name__ == "__main__":
    main()
