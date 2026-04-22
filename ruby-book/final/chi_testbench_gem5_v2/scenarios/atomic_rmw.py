# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
atomic_rmw — The "fancy" CHI transaction: a bare CleanUnique ownership
upgrade emitted directly on the wire by role C, followed by a real
WriteUniqueFull. Tiles A and B pre-populate Shared copies so C's
CleanUnique forces a SnpCleanInvalid fan-out — a traffic shape that
v1's CPU-side stimulus cannot produce directly because the sequencer
collapses the MemCmd flavors that would otherwise lead to CleanUnique.

Role A (tile 0)  : ReadShared
Role B (tile 8)  : ReadShared
Role C (tile 15) : CleanUnique → WriteUniqueFull
Role A           : ReadShared (re-read)
"""

import m5
from m5.objects import (
    ChiDriverNode,
    ChiGem5V2Barrier,
    ChiGem5V2EventBus,
)


def build(args, planner):
    if args.rn_mode == "rni":
        m5.fatal(
            "atomic_rmw exercises SnpCleanInvalid fan-out via the tile's "
            "cache (rnf_l2). --rn-mode=rni has no tile cache; re-run "
            "with --rn-mode=rnf_l2."
        )
    target_addr = planner.address_for_hnf(hnf_idx=9, line_offset=0)
    barrier = ChiGem5V2Barrier(expected=3)
    bus = ChiGem5V2EventBus()

    drivers = [None] * args.num_cpus

    def _role(tile, role_name):
        return ChiDriverNode(
            sequence="atomic_rmw",
            tile_id=tile,
            target_addr=target_addr,
            access_size=64,
            role=role_name,
            finish_barrier=barrier,
            event_bus=bus,
        )

    drivers[0] = _role(0, "A")
    drivers[8] = _role(8, "B")
    drivers[15] = _role(15, "C")

    for tile in range(args.num_cpus):
        if drivers[tile] is None:
            drivers[tile] = ChiDriverNode(sequence="idle", tile_id=tile)
    return drivers
