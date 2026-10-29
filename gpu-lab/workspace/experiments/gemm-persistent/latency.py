#!/usr/bin/env python3
"""Global load/store issue-to-completion latency from a GPUTrace,GPUMem log."""

import re
import statistics
import sys
from collections import defaultdict

prefix = r"^\s*(\d+): .*CU(\d+): WF\[(\d+)\]\[(\d+)\]: "
issue = re.compile(
    prefix + r"wave\[(\d+)\] Executing inst: (global_load\S*|global_store\S*)"
)
complete = re.compile(
    prefix + r"Completing global mem instr (global_load\S*|global_store\S*)"
)
pending = defaultdict(list)
lat = {"load": [], "store": []}
with open(sys.argv[1]) as f:
    for line in f:
        m = issue.search(line)
        if m:
            tick, cu, simd, slot, dyn, inst = m.groups()
            pending[(cu, simd, slot, inst.split("_")[1][:5])].append(int(tick))
            continue
        m = complete.search(line)
        if m:
            tick, cu, simd, slot, inst = m.groups()
            kind = inst.split("_")[1][:5]
            q = pending[(cu, simd, slot, kind)]
            if q:
                lat[
                    (
                        "load"
                        if kind == "load_" or kind.startswith("load")
                        else "store"
                    )
                ].append((int(tick) - q.pop(0)) / 476)
for k, v in lat.items():
    if v:
        v.sort()
        print(
            k,
            "n",
            len(v),
            "min",
            round(v[0]),
            "median",
            round(statistics.median(v)),
            "p90",
            round(v[int(len(v) * 0.9)]),
            "max",
            round(v[-1]),
            "cycles",
        )
