# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
memset — Tile 7's driver pipelines async CHI WriteUniqueFull bursts.
dst_base=0 so the destination stripes across all 16 HNFs.
"""

from m5.objects import (
    ChiDriverNode,
    ChiGem5V2Barrier,
)


def build(args, planner):
    num_lines = 256
    dst_base = 0
    barrier = ChiGem5V2Barrier(expected=1)

    drivers = [None] * args.num_cpus
    drivers[7] = ChiDriverNode(
        sequence="memset",
        tile_id=7,
        dst_base=dst_base,
        num_lines=num_lines,
        line_size=64,
        pipeline_depth=4,
        fill_byte=0xCC,
        finish_barrier=barrier,
    )
    for tile in range(args.num_cpus):
        if drivers[tile] is None:
            drivers[tile] = ChiDriverNode(sequence="idle", tile_id=tile)
    return drivers
