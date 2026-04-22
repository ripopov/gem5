# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
read_ex_walk — Three-tile CHI coherence walk. Tiles A and B take a
Shared copy; tile C's WriteUniqueFull invalidates both of them.

Role A (tile 0)  : ReadShared             → tile-A SC
Role B (tile 8)  : ReadShared             → tile-B SC
Role C (tile 15) : WriteUniqueFull        → invalidates tile-A and tile-B
Role A           : ReadShared (re-read)   → tile-A refills from HN
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
            "read_ex_walk depends on tile-side cache state transitions "
            "(Shared→Invalid via SnpCleanInvalid). --rn-mode=rni has no "
            "tile cache. Re-run with --rn-mode=rnf_l2."
        )
    target_addr = planner.address_for_hnf(hnf_idx=5, line_offset=0)
    barrier = ChiGem5V2Barrier(expected=3)
    bus = ChiGem5V2EventBus()

    drivers = [None] * args.num_cpus

    def _role(tile, role_name):
        return ChiDriverNode(
            sequence="read_ex_walk",
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
