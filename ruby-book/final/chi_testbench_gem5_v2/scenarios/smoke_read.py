# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
smoke_read — Tile 0's driver issues CHI ReadShared to one line homed
at every HNF slice, `scenario-iterations` times. Other tiles run idle.

This is the v2 analog of v1's smoke_read: same address set, same line
counts, but the stimulus is a direct CHI ReadShared on the wire rather
than a Packet through a RubySequencer.
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
                    sequence="smoke_read",
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
