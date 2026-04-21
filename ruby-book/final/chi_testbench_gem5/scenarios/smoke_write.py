# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
smoke_write — Tile 0 writes one line per HNF slice, repeatedly.
"""

from m5.objects import (
    ChiGem5Barrier,
    ChiSeqDriver,
)


def build(args, planner):
    addresses = planner.one_line_per_hnf(line_offset=0)
    barrier = ChiGem5Barrier(expected=1)

    drivers = []
    for tile in range(args.num_cpus):
        if tile == 0:
            drivers.append(
                ChiSeqDriver(
                    sequence="smoke_write",
                    tile_id=tile,
                    addresses=addresses,
                    access_size=8,
                    iterations=args.scenario_iterations,
                    finish_barrier=barrier,
                )
            )
        else:
            drivers.append(ChiSeqDriver(sequence="idle", tile_id=tile))
    return drivers
