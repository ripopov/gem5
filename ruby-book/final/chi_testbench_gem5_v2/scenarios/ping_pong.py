# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
ping_pong — Tiles 0 and 15 take turns sending CHI WriteUniqueFull to a
shared cache line homed at HNF 7. Latch hand-off via ChiGem5V2EventBus.
"""

from m5.objects import (
    ChiDriverNode,
    ChiGem5V2Barrier,
    ChiGem5V2EventBus,
)


def build(args, planner):
    line_addr = planner.address_for_hnf(hnf_idx=7, line_offset=0)
    barrier = ChiGem5V2Barrier(expected=2)
    bus = ChiGem5V2EventBus()
    iterations = max(args.scenario_iterations, 100)

    drivers = [None] * args.num_cpus
    drivers[0] = ChiDriverNode(
        sequence="ping_pong",
        tile_id=0,
        line_addr=line_addr,
        access_size=8,
        iterations=iterations,
        initiator=True,
        wait_event_name="turn_a",
        post_event_name="turn_b",
        finish_barrier=barrier,
        event_bus=bus,
    )
    drivers[15] = ChiDriverNode(
        sequence="ping_pong",
        tile_id=15,
        line_addr=line_addr,
        access_size=8,
        iterations=iterations,
        initiator=False,
        wait_event_name="turn_b",
        post_event_name="turn_a",
        finish_barrier=barrier,
        event_bus=bus,
    )
    for tile in range(args.num_cpus):
        if drivers[tile] is None:
            drivers[tile] = ChiDriverNode(sequence="idle", tile_id=tile)
    return drivers
