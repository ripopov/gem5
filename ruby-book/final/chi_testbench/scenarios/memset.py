# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
memset — Stage-2 pipelined burst writes.

Tile 7 writes a fill byte to `num_lines` cache lines spanning all 16
HNF slices (via the interleaving addressing). `pipeline_depth` stores
are in flight concurrently.

Exercises the ReadUnique -> WriteBackFull path under a steady
offered-load regime.
"""

from m5.objects import (
    ChiFinishBarrier,
    IdleDriver,
    MemsetDriver,
)


def build(args, planner):
    num_lines = 256
    # Use line-0 of each HNF stripe so the destination sweeps all 16
    # HNFs at the cache-line interleave granularity.
    dst_base = 0
    barrier = ChiFinishBarrier(expected=1)

    drivers = [None] * args.num_cpus
    drivers[7] = MemsetDriver(
        dst_base=dst_base,
        num_lines=num_lines,
        line_size=64,
        pipeline_depth=4,
        fill_byte=0xCC,
        finish_barrier=barrier,
    )
    for tile in range(args.num_cpus):
        if drivers[tile] is None:
            drivers[tile] = IdleDriver()
    return drivers
