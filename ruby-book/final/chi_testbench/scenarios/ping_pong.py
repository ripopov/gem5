# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
ping_pong — Stage-2 deterministic two-tile ownership ping-pong.

Tile 0 and tile 15 alternate exclusive writes to a single cache line
homed at HNF 7 (mid-mesh). A shared ChiEventBus carries the two
hand-off events.

What to check:
- Driver-side mean per-round write latency (from "my turn starts" to
  "my write has a response") stays roughly constant across iterations
  after the first cold transfer.
- stats.txt: SnpUnique count on the shared line's HNF is ~2 x iterations.
- stats.txt: diagonal-path Garnet links carry non-trivial flit traffic.
"""

from m5.objects import (
    ChiEventBus,
    ChiFinishBarrier,
    IdleDriver,
    PingPongDriver,
)


def build(args, planner):
    line_addr = planner.address_for_hnf(hnf_idx=7, line_offset=0)
    bus = ChiEventBus()
    barrier = ChiFinishBarrier(expected=2)
    iterations = max(args.scenario_iterations, 100)

    drivers = [None] * args.num_cpus
    drivers[0] = PingPongDriver(
        line_addr=line_addr,
        access_size=8,
        iterations=iterations,
        initiator=True,
        wait_event="turn_a",
        post_event="turn_b",
        bus=bus,
        finish_barrier=barrier,
    )
    drivers[15] = PingPongDriver(
        line_addr=line_addr,
        access_size=8,
        iterations=iterations,
        initiator=False,
        wait_event="turn_b",
        post_event="turn_a",
        bus=bus,
        finish_barrier=barrier,
    )
    for tile in range(args.num_cpus):
        if drivers[tile] is None:
            drivers[tile] = IdleDriver()
    return drivers
