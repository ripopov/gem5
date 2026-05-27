# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
ping_pong — Tiles 0 and 15 take turns writing one shared cache line
homed at HNF 7. The turn hand-off lives in byte 0 of that line, matching
a CPU-style spin-on-shared-memory ping-pong test.
"""

import m5
from m5.objects import (
    ChiGem5Barrier,
    ChiSeqDriver,
    PingPongSequence,
)


def build(args, planner):
    line_addr = planner.address_for_hnf(hnf_idx=7, line_offset=0)
    if args.active_cores_spec.lower() == "all":
        active_cores = [0, args.num_cpus - 1]
    else:
        active_cores = list(args.active_cores)
        if len(active_cores) != 2:
            m5.fatal(
                "--active-cores selects ping_pong endpoints and must "
                "contain exactly two tile IDs"
            )

    barrier = ChiGem5Barrier(expected=2)
    iterations = max(args.scenario_iterations, 100)
    initiator_tile, responder_tile = active_cores

    drivers = [None] * args.num_cpus
    drivers[initiator_tile] = ChiSeqDriver(
        tile_id=initiator_tile,
        finish_barrier=barrier,
        sequence=PingPongSequence(
            line_addr=line_addr,
            iterations=iterations,
            initiator=True,
            l3_clock=args.ruby_clock,
            roi_iteration=0,
            dump_roi_stats=True,
        ),
    )
    drivers[responder_tile] = ChiSeqDriver(
        tile_id=responder_tile,
        finish_barrier=barrier,
        sequence=PingPongSequence(
            line_addr=line_addr,
            iterations=iterations,
            initiator=False,
            l3_clock=args.ruby_clock,
            roi_iteration=0,
            dump_roi_stats=True,
        ),
    )
    for tile in range(args.num_cpus):
        if drivers[tile] is None:
            drivers[tile] = ChiSeqDriver(tile_id=tile)
    return drivers
