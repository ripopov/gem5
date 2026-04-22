# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
idle — every tile's driver runs the `idle` sequence (no CHI traffic).

Used as the skeleton smoke-test: verifies the v2 build links, SimObjects
instantiate, the mesh + HN-Fs + drivers wire up, and every driver's
fiber enters + returns + signals the barrier cleanly.
"""

from m5.objects import (
    ChiDriverNode,
    ChiGem5V2Barrier,
)


def build(args, planner):
    barrier = ChiGem5V2Barrier(expected=args.num_cpus)

    drivers = []
    for tile in range(args.num_cpus):
        drivers.append(
            ChiDriverNode(
                sequence="idle",
                tile_id=tile,
                finish_barrier=barrier,
            )
        )
    return drivers
