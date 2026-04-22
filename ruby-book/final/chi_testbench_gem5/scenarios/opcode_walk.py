# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
opcode_walk — Tile 3 cold-read → write-pattern → evict → re-read with
data-dependent assertions.
"""

import m5
from m5.objects import (
    ChiGem5Barrier,
    ChiSeqDriver,
)


def build(args, planner):
    if args.rn_mode == "rni":
        m5.fatal(
            "opcode_walk asserts CHI opcode sequences that only emerge "
            "from RN-side cache state transitions (ReadShared, ReadUnique, "
            "CleanUnique). Re-run with --rn-mode=rnf_l2."
        )
    target_addr = planner.address_for_hnf(hnf_idx=3, line_offset=0)
    filler_base = planner.address_for_hnf(hnf_idx=0, line_offset=16)
    barrier = ChiGem5Barrier(expected=1)

    drivers = [None] * args.num_cpus
    drivers[3] = ChiSeqDriver(
        sequence="opcode_walk",
        tile_id=3,
        target_addr=target_addr,
        filler_base=filler_base,
        filler_lines=2048,
        finish_barrier=barrier,
    )
    for tile in range(args.num_cpus):
        if drivers[tile] is None:
            drivers[tile] = ChiSeqDriver(sequence="idle", tile_id=tile)
    return drivers
