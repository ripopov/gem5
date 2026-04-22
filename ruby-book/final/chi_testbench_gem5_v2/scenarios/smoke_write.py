# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
smoke_write — Tile 0's driver issues CHI WriteUniqueFull to one line
homed at every HNF slice. Other tiles run idle.
"""

from m5.objects import (
    ChiDriverNode,
    ChiGem5V2Barrier,
)


def build(args, planner):
    addresses = planner.one_line_per_hnf(line_offset=0)
    barrier = ChiGem5V2Barrier(expected=1)

    drivers = []
    for tile in range(args.num_cpus):
        if tile == 0:
            drivers.append(
                ChiDriverNode(
                    sequence="smoke_write",
                    tile_id=tile,
                    addresses=addresses,
                    access_size=8,
                    iterations=args.scenario_iterations,
                    finish_barrier=barrier,
                )
            )
        else:
            drivers.append(ChiDriverNode(sequence="idle", tile_id=tile))
    return drivers
