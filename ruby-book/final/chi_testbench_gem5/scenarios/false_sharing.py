# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
false_sharing — Tiles 0 and 15 each write a different byte of the same
cache line, repeatedly. Semantically disjoint bytes, but every write
by one tile invalidates the other's copy.
"""

from m5.objects import (
    ChiGem5Barrier,
    ChiSeqDriver,
    FalseSharingSequence,
)


def build(args, planner):
    line_addr = planner.address_for_hnf(hnf_idx=7, line_offset=0)
    barrier = ChiGem5Barrier(expected=2)
    iterations = max(args.scenario_iterations, 1000)

    drivers = [None] * args.num_cpus
    drivers[0] = ChiSeqDriver(
        tile_id=0,
        finish_barrier=barrier,
        sequence=FalseSharingSequence(
            line_addr=line_addr,
            byte_offset=0,
            iterations=iterations,
        ),
    )
    drivers[15] = ChiSeqDriver(
        tile_id=15,
        finish_barrier=barrier,
        sequence=FalseSharingSequence(
            line_addr=line_addr,
            byte_offset=32,
            iterations=iterations,
        ),
    )
    for tile in range(args.num_cpus):
        if drivers[tile] is None:
            drivers[tile] = ChiSeqDriver(tile_id=tile)
    return drivers
