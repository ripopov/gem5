# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
read_ex_walk — Exercises MemCmd::ReadExReq. Three roles on three
tiles, coordinated via ChiGem5EventBus latches.

Role A (tile 0) : plain read  → ReadShared
Role B (tile 8) : plain read  → ReadShared (after A)
Role C (tile 15): read_exclusive → ReadUnique (invalidates A and B),
                   then a local write that hits the already-Unique line.

Observable signatures (compare to a run without role C):
  - Extra ReadUnique + SnpUnique pair in CHI counters.
  - A's post-invalidation re-read is a fresh miss.
  - C's write emits zero additional HNF transactions.
"""

import m5
from m5.objects import (
    ChiGem5Barrier,
    ChiGem5EventBus,
    ChiSeqDriver,
)


def build(args, planner):
    if args.rn_mode == "rni":
        m5.fatal(
            "read_ex_walk demonstrates Shared->Unique ownership upgrade "
            "via CleanUnique on the RN-side leaf cache. --rn-mode=rni "
            "has no RN cache, so there is nothing to upgrade. Re-run "
            "with --rn-mode=rnf_l2."
        )
    target_addr = planner.address_for_hnf(hnf_idx=5, line_offset=0)
    barrier = ChiGem5Barrier(expected=3)
    bus = ChiGem5EventBus()

    drivers = [None] * args.num_cpus

    def _role(tile, role_name):
        return ChiSeqDriver(
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
            drivers[tile] = ChiSeqDriver(sequence="idle", tile_id=tile)
    return drivers
