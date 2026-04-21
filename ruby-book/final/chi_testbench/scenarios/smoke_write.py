# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
smoke_write — stage-1 smoke test.

Tile 0 writes one cache line homed at each of the 16 HNF slices,
`iterations` times. Exercises the ReadUnique -> WriteBackFull path
at every slice; all other tiles are idle.
"""

from m5.objects import (
    IdleDriver,
    SmokeWriteDriver,
)


def build(args, planner):
    addresses = planner.one_line_per_hnf(line_offset=0)

    drivers = []
    for tile in range(args.num_cpus):
        if tile == 0:
            drivers.append(
                SmokeWriteDriver(
                    addresses=addresses,
                    access_size=8,
                    iterations=args.scenario_iterations,
                    stop_on_finish=True,
                )
            )
        else:
            drivers.append(IdleDriver())
    return drivers
