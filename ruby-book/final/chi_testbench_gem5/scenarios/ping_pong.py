# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
ping_pong — Tiles 0 and 15 take turns writing one shared cache line
homed at HNF 7. Latch hand-off via ChiGem5EventBus. The initiator writes
first; the non-initiator waits for the first notify.
"""

from m5.objects import (
    ChiGem5Barrier,
    ChiGem5EventBus,
    ChiSeqDriver,
    PingPongSequence,
)


def build(args, planner):
    line_addr = planner.address_for_hnf(hnf_idx=7, line_offset=0)
    barrier = ChiGem5Barrier(expected=2)
    bus = ChiGem5EventBus()
    iterations = max(args.scenario_iterations, 100)

    drivers = [None] * args.num_cpus
    drivers[0] = ChiSeqDriver(
        tile_id=0,
        finish_barrier=barrier,
        event_bus=bus,
        sequence=PingPongSequence(
            line_addr=line_addr,
            access_size=8,
            iterations=iterations,
            initiator=True,
            wait_event_name="turn_a",
            post_event_name="turn_b",
        ),
    )
    drivers[15] = ChiSeqDriver(
        tile_id=15,
        finish_barrier=barrier,
        event_bus=bus,
        sequence=PingPongSequence(
            line_addr=line_addr,
            access_size=8,
            iterations=iterations,
            initiator=False,
            wait_event_name="turn_b",
            post_event_name="turn_a",
        ),
    )
    for tile in range(args.num_cpus):
        if drivers[tile] is None:
            drivers[tile] = ChiSeqDriver(tile_id=tile)
    return drivers
