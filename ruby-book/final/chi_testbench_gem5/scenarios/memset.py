# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""memset — selected tiles write non-overlapping linear ranges."""

from m5.objects import (
    ChiGem5Barrier,
    ChiSeqDriver,
    MemsetSequence,
)


def build(args, planner):
    num_lines = 1024
    line_size = 64
    write_size = 63
    stats_quiesce_cycles = 10000
    cl_stride = getattr(args, "cl_stride", 1)
    active_cores = list(args.active_cores)
    barrier = ChiGem5Barrier(expected=len(active_cores))

    drivers = [
        ChiSeqDriver(
            tile_id=tile,
            sequence=MemsetSequence(
                num_lines=0,
                num_outstanding_reqs=args.num_outstanding_reqs,
            ),
        )
        for tile in range(args.num_cpus)
    ]
    for tile in active_cores:
        drivers[tile] = (
            ChiSeqDriver(
                tile_id=tile,
                finish_barrier=barrier,
                sequence=MemsetSequence(
                    dst_base=tile * num_lines * cl_stride * line_size,
                    num_lines=num_lines,
                    line_size=line_size,
                    cl_stride=cl_stride,
                    write_size=write_size,
                    num_outstanding_reqs=args.num_outstanding_reqs,
                    operation=getattr(args, "operation", "store"),
                    fill_byte=(0xC0 + tile) & 0xFF,
                    warmup_l3=True,
                    roi_stats=True,
                    roi_participants=len(active_cores),
                    stats_quiesce_cycles=stats_quiesce_cycles,
                ),
            )
        )
    return drivers
