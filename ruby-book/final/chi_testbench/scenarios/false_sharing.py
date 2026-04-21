# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
false_sharing — Stage-2 same-line adjacent-byte contention.

Tile 0 and tile 15 each repeatedly write ONE byte of a shared cache
line, at different byte offsets. The writes are semantically
independent (different bytes) but the cache protocol sees shared-line
contention: every write by one tile invalidates the other's copy.

What to check:
- stats.txt: SNP vnet flit count is high relative to smoke scenarios.
- Per-HNF demand-misses on the shared line's home are ~= 2 x iterations.
- Diagonal-path Garnet links show elevated traffic.
"""

from m5.objects import (
    ChiFinishBarrier,
    FalseSharingDriver,
    IdleDriver,
)


def build(args, planner):
    line_addr = planner.address_for_hnf(hnf_idx=7, line_offset=0)
    barrier = ChiFinishBarrier(expected=2)
    iterations = max(args.scenario_iterations, 1000)

    drivers = [None] * args.num_cpus
    drivers[0] = FalseSharingDriver(
        line_addr=line_addr,
        byte_offset=0,
        iterations=iterations,
        finish_barrier=barrier,
    )
    drivers[15] = FalseSharingDriver(
        line_addr=line_addr,
        byte_offset=32,  # same line, non-overlapping word
        iterations=iterations,
        finish_barrier=barrier,
    )
    for tile in range(args.num_cpus):
        if drivers[tile] is None:
            drivers[tile] = IdleDriver()
    return drivers
