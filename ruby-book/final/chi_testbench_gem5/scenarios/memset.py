# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""memset — all tiles write non-overlapping linear ranges."""

from m5.objects import (
    ChiGem5Barrier,
    ChiSeqDriver,
    MemsetSequence,
)


def build(args, planner):
    num_lines = 1024
    line_size = 64
    write_size = 63
    barrier = ChiGem5Barrier(expected=args.num_cpus)

    drivers = []
    for tile in range(args.num_cpus):
        drivers.append(
            ChiSeqDriver(
                tile_id=tile,
                finish_barrier=barrier,
                sequence=MemsetSequence(
                    dst_base=tile * num_lines * line_size,
                    num_lines=num_lines,
                    line_size=line_size,
                    write_size=write_size,
                    pipeline_depth=4,
                    fill_byte=(0xC0 + tile) & 0xFF,
                    roi_stats=True,
                    roi_participants=args.num_cpus,
                ),
            )
        )
    return drivers
