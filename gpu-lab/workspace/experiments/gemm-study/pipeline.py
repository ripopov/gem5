#!/usr/bin/env python3
"""Find compute issued while a same-wave global load awaits completion.

Read a gem5 GPUTrace,GPUMem log. This establishes dynamic overlap; correlate
the reported PCs with saved ISA to identify the future tile being prefetched.
The CU-wide count establishes workgroup overlap only for one workgroup per CU.
"""

import argparse
import json
import re
from collections import defaultdict
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    args = parser.parse_args()
    prefix = r"^\s*(\d+): .*CU(\d+): WF\[(\d+)\]\[(\d+)\]: "
    issue = re.compile(
        prefix + r"wave\[\d+\] Executing inst: (.*?) "
        r"\(pc: (0x[0-9a-f]+); seqNum: (\d+)\)"
    )
    complete = re.compile(prefix + r"Completing (global|local) mem instr (.*)")
    pending = defaultdict(list)
    pending_lds = defaultdict(list)
    examples = []
    loads = computes = overlap = cu_overlap = unmatched = 0
    lds_loads = lds_overlap = 0
    with args.trace.open() as stream:
        for line in stream:
            match = issue.search(line)
            if match:
                tick, cu, simd, slot, instruction, pc, seq = match.groups()
                wave = (cu, simd, slot)
                if instruction.startswith("global_load"):
                    loads += 1
                    pending[wave].append(
                        dict(
                            instruction=instruction,
                            pc=pc,
                            seq=int(seq),
                            issue_tick=int(tick),
                        )
                    )
                elif instruction.startswith("ds_read"):
                    lds_loads += 1
                    pending_lds[wave].append(
                        dict(
                            instruction=instruction,
                            pc=pc,
                            seq=int(seq),
                            issue_tick=int(tick),
                        )
                    )
                elif instruction.startswith(("v_mfma", "v_fma")):
                    computes += 1
                    if pending_lds[wave]:
                        lds_overlap += 1
                    cu_pending = [
                        (key, entry)
                        for key, entries in pending.items()
                        if key[0] == cu
                        for entry in entries
                    ]
                    if cu_pending:
                        cu_overlap += 1
                    if pending[wave]:
                        overlap += 1
                    if cu_pending and len(examples) < 8:
                        load_wave, load = cu_pending[0]
                        examples.append(
                            dict(
                                compute_wave=wave,
                                compute_pc=pc,
                                compute_tick=int(tick),
                                load_wave=load_wave,
                                load=load,
                            )
                        )
                continue
            match = complete.search(line)
            if match:
                tick, cu, simd, slot, space, instruction = match.groups()
                expected = "global_load" if space == "global" else "ds_read"
                if not instruction.startswith(expected):
                    continue
                queue = pending if space == "global" else pending_lds
                entries = queue[(cu, simd, slot)]
                for index, entry in enumerate(entries):
                    if entry["instruction"] == instruction:
                        entry["complete_tick"] = int(tick)
                        entries.pop(index)
                        break
                else:
                    unmatched += 1
    result = dict(
        loads=loads,
        compute_instructions=computes,
        compute_with_pending_global_load=overlap,
        compute_with_pending_cu_global_load=cu_overlap,
        lds_loads=lds_loads,
        compute_with_pending_lds_load=lds_overlap,
        unmatched_completions=unmatched,
        unfinished_loads=sum(map(len, pending.values())),
        unfinished_lds_loads=sum(map(len, pending_lds.values())),
        examples=examples,
    )
    print(json.dumps(result, indent=2))
    if (
        not loads
        or not computes
        or unmatched
        or result["unfinished_loads"]
        or result["unfinished_lds_loads"]
    ):
        parser.error("trace is incomplete or could not be matched")


if __name__ == "__main__":
    main()
