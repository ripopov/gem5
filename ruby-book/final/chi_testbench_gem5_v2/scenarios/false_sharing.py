# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
false_sharing — Tiles 0 and 15 each write a different byte of the same
cache line, repeatedly. Semantically disjoint bytes, but every write
is coherent on the full line, so each side snoop-invalidates the
other.
"""

from m5.objects import (
    ChiDriverNode,
    ChiGem5V2Barrier,
)


def build(args, planner):
    line_addr = planner.address_for_hnf(hnf_idx=7, line_offset=0)
    barrier = ChiGem5V2Barrier(expected=2)
    iterations = max(args.scenario_iterations, 1000)

    drivers = [None] * args.num_cpus
    drivers[0] = ChiDriverNode(
        sequence="false_sharing",
        tile_id=0,
        line_addr=line_addr,
        byte_offset=0,
        iterations=iterations,
        finish_barrier=barrier,
    )
    drivers[15] = ChiDriverNode(
        sequence="false_sharing",
        tile_id=15,
        line_addr=line_addr,
        byte_offset=32,
        iterations=iterations,
        finish_barrier=barrier,
    )
    for tile in range(args.num_cpus):
        if drivers[tile] is None:
            drivers[tile] = ChiDriverNode(sequence="idle", tile_id=tile)
    return drivers
