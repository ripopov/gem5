#!/usr/bin/env python3
"""Tabulate GPU cycles and selected counters for completed study variants."""

import glob
import importlib.util
import json
import sys
from pathlib import Path

WORKSPACE = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "collect", WORKSPACE / "experiments/gemm-study/collect.py"
)
collect = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collect)


def main():
    names = sys.argv[1:]
    keys = [
        "globalReads",
        "globalWrites",
        "groupReads",
        "groupWrites",
        "extra_bank_serialization",
        "wave_slot_cycles_WaitCnt",
        "wave_slot_cycles_BarrierWait",
        "wave_slot_cycles_MatrixCore",
        "vALUInsts",
        "sALUInsts",
    ]
    print("variant cycles dramRead dramWrite " + " ".join(keys))
    for name in names:
        for path in sorted(
            glob.glob(
                str(WORKSPACE / "work/gemm-study" / name / "result-*.json")
            )
        ):
            r = json.loads(Path(path).read_text())
            if r.get("gpu_active_cycles") is None:
                continue
            run = WORKSPACE / "work/runs" / r["run"]
            c = collect.counters(run)
            d = r["whole_run_dram"]
            print(
                name,
                round(r["gpu_active_cycles"]),
                int(d["system.gpu_mem_ctrls.dram.dramBytesRead"]),
                int(d["system.gpu_mem_ctrls.dram.dramBytesWritten"]),
                " ".join(str(int(c[k])) for k in keys),
            )


if __name__ == "__main__":
    main()
