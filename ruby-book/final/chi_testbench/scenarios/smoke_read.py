# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
smoke_read — Stage-1 smoke test.

Tile 0 issues a blocking read to one cache line homed at each of the
16 HNF slices, `iterations` times. Every other tile hosts an idle
driver (empty address list, does not call sc_stop).

What to check:
- Simulation completes.
- stats.txt shows non-zero m_demand_hits/m_demand_misses at every HNF
  controller, roughly balanced.
"""

from m5.objects import SmokeReadDriver


def build(args, planner):
    addresses = planner.one_line_per_hnf(line_offset=0)

    drivers = []
    for tile in range(args.num_cpus):
        if tile == 0:
            drivers.append(
                SmokeReadDriver(
                    addresses=addresses,
                    access_size=8,
                    iterations=args.scenario_iterations,
                    stop_on_finish=True,
                )
            )
        else:
            drivers.append(
                SmokeReadDriver(
                    addresses=[],
                    access_size=8,
                    iterations=0,
                    stop_on_finish=False,
                )
            )
    return drivers
