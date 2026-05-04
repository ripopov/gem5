# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
memset — Tile 7 pipelined burst writes. dst_base=0 so the destination
interleaves across all 16 HNFs.
"""

from m5.objects import (
    ChiGem5Barrier,
    ChiSeqDriver,
    MemsetSequence,
)


def build(args, planner):
    num_lines = 256
    dst_base = 0
    barrier = ChiGem5Barrier(expected=1)

    drivers = [None] * args.num_cpus
    drivers[7] = ChiSeqDriver(
        tile_id=7,
        finish_barrier=barrier,
        sequence=MemsetSequence(
            dst_base=dst_base,
            num_lines=num_lines,
            line_size=64,
            pipeline_depth=4,
            fill_byte=0xCC,
        ),
    )
    for tile in range(args.num_cpus):
        if drivers[tile] is None:
            drivers[tile] = ChiSeqDriver(tile_id=tile)
    return drivers
