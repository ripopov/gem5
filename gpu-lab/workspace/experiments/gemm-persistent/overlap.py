#!/usr/bin/env python3
"""Measure cross-tile overlap in a persistent GEMM trace.

Reads a gem5 log produced with --debug-flags=GPUTrace,GPUMem. Roles are
inferred from behaviour: a wave that issues MFMA instructions is a compute
wave, a wave that issues only global loads is a producer, and other waves
(runtime blit kernels) are ignored. Compute waves of one CU are ranked by
dynamic wave id, which the dispatcher assigns in thread order, so with
--compute-waves W and --groups G consecutive blocks of W ranks form groups
0..G-1 of successive workgroups. The script reports, per CU, how many MFMA
instructions issued while

  * a global store from a wave of a different compute group was pending
    (output stores overlapping another group's computation);
  * a global store from the same wave was pending (stores overlapping the
    same wave's next tile);
  * a global load from a producer wave of the same CU was pending.

Pending means issued and not yet reported complete. All loads and stores must
match a completion, otherwise the trace is rejected.
"""

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument(
        "--compute-waves",
        type=int,
        required=True,
        help="waves per compute group",
    )
    parser.add_argument("--groups", type=int, default=1)
    args = parser.parse_args()
    prefix = r"^\s*(\d+): .*CU(\d+): WF\[(\d+)\]\[(\d+)\]: "
    issue = re.compile(prefix + r"wave\[(\d+)\] Executing inst: (\S+)")
    complete = re.compile(
        prefix + r"Completing (global|local) mem instr (\S+)"
    )
    # First pass: classify every dynamic wave by the instructions it issues.
    kinds = defaultdict(set)  # (cu, dyn) -> {"mfma", "load", "store"}
    with args.trace.open() as stream:
        for line in stream:
            match = issue.search(line)
            if match:
                _, cu, _, _, dyn, inst = match.groups()
                if inst.startswith("v_mfma"):
                    kinds[(cu, int(dyn))].add("mfma")
                elif inst.startswith("global_load"):
                    kinds[(cu, int(dyn))].add("load")
                elif inst.startswith("global_store"):
                    kinds[(cu, int(dyn))].add("store")
    group_of = {}  # (cu, dyn) -> group index or "producer" or None
    per_cu = defaultdict(list)
    for (cu, dyn), k in kinds.items():
        per_cu[cu].append(dyn)
        if "mfma" in k:
            group_of[(cu, dyn)] = "compute"
        elif k == {"load"}:
            group_of[(cu, dyn)] = "producer"
        else:
            group_of[(cu, dyn)] = None
    for cu, dyns in per_cu.items():
        compute = sorted(d for d in dyns if group_of[(cu, d)] == "compute")
        for rank, d in enumerate(compute):
            group_of[(cu, d)] = (rank // args.compute_waves) % args.groups
    role = {}  # wave key -> (cu, group or "producer", dyn id)
    pending_store = defaultdict(list)  # wave key -> issue ticks
    pending_load = defaultdict(list)
    stats = defaultdict(lambda: defaultdict(int))
    span = {}  # cu -> [first MFMA tick, last MFMA tick, last store tick]
    tile_stores = defaultdict(int)
    unmatched = 0
    first_tick = last_tick = None
    with args.trace.open() as stream:
        for line in stream:
            match = issue.search(line)
            if match:
                tick, cu, simd, slot, dyn, inst = match.groups()
                tick = int(tick)
                first_tick = tick if first_tick is None else first_tick
                last_tick = tick
                key = (cu, simd, slot)
                dyn = int(dyn)
                group = group_of.get((cu, dyn))
                if group is None:
                    # Runtime blit waves reuse slots; drop their completions.
                    role[key] = None
                    continue
                role[key] = (cu, group, dyn)
                s = stats[cu]
                sp = span.setdefault(cu, [None, None, None])
                if inst.startswith("global_store"):
                    pending_store[key].append(tick)
                    s["stores"] += 1
                    sp[2] = tick
                elif inst.startswith("global_load"):
                    pending_load[key].append(tick)
                    s["loads"] += 1
                elif inst.startswith("v_mfma"):
                    s["mfma"] += 1
                    sp[0] = tick if sp[0] is None else sp[0]
                    sp[1] = tick
                    other = same = producer = False
                    for other_key, ticks in pending_store.items():
                        if (
                            not ticks
                            or role[other_key] is None
                            or role[other_key][0] != cu
                        ):
                            continue
                        if other_key == key:
                            same = True
                        elif role[other_key][1] != group:
                            other = True
                    for other_key, ticks in pending_load.items():
                        if (
                            ticks
                            and role[other_key] is not None
                            and role[other_key][0] == cu
                            and role[other_key][1] == "producer"
                        ):
                            producer = True
                    s["mfma_with_other_group_store_pending"] += other
                    s["mfma_with_same_wave_store_pending"] += same
                    s["mfma_with_producer_load_pending"] += producer
                continue
            match = complete.search(line)
            if match:
                tick, cu, simd, slot, space, inst = match.groups()
                key = (cu, simd, slot)
                if space != "global" or role.get(key) is None:
                    continue
                queue = (
                    pending_store
                    if inst.startswith("global_store")
                    else (
                        pending_load
                        if inst.startswith("global_load")
                        else None
                    )
                )
                if queue is None:
                    continue
                if queue[key]:
                    queue[key].pop(0)
                else:
                    unmatched += 1
    unfinished = sum(map(len, pending_store.values())) + sum(
        map(len, pending_load.values())
    )
    groups_seen = defaultdict(set)
    for entry in role.values():
        if entry is not None:
            groups_seen[entry[0]].add(entry[1])
    for cu, sp in span.items():
        stats[cu]["first_mfma_tick"] = sp[0]
        stats[cu]["last_mfma_tick"] = sp[1]
        stats[cu]["last_store_tick"] = sp[2]
    result = {
        "cus": {cu: dict(s) for cu, s in sorted(stats.items())},
        "roles_per_cu": {
            cu: sorted(map(str, g)) for cu, g in sorted(groups_seen.items())
        },
        "unmatched_completions": unmatched,
        "unfinished_memory_instructions": unfinished,
        "trace_ticks": (last_tick - first_tick) if first_tick else 0,
    }
    totals = defaultdict(int)
    for s in stats.values():
        for k, v in s.items():
            if not k.endswith("_tick"):
                totals[k] += v
    result["totals"] = dict(totals)
    print(json.dumps(result, indent=2))
    if unmatched or unfinished or not totals.get("mfma"):
        parser.error("trace incomplete or unmatched")


if __name__ == "__main__":
    main()
