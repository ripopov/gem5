# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
memcpy — Tile 7 pipelined copy from an HNF-0 range to an HNF-15 range.
"""

from m5.objects import (
    ChiGem5Barrier,
    ChiSeqDriver,
)


def build(args, planner):
    num_lines = 256
    src_base = planner.address_for_hnf(hnf_idx=0, line_offset=0)
    dst_base = planner.address_for_hnf(hnf_idx=15, line_offset=0)
    barrier = ChiGem5Barrier(expected=1)

    drivers = [None] * args.num_cpus
    drivers[7] = ChiSeqDriver(
        sequence="memcpy",
        tile_id=7,
        src_base=src_base,
        dst_base=dst_base,
        num_lines=num_lines,
        line_size=64,
        pipeline_depth=4,
        finish_barrier=barrier,
    )
    for tile in range(args.num_cpus):
        if drivers[tile] is None:
            drivers[tile] = ChiSeqDriver(sequence="idle", tile_id=tile)
    return drivers
