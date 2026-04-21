# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
smoke_opcode_mix — stage-1 smoke test.

One SmokeOpcodeMixDriver per tile, all running concurrently. Each
driver hits a private address stripe (so no false sharing), mixing
65% reads and 35% writes over `scenario-iterations` passes. This
exercises the full CHI demand opcode envelope simultaneously:
ReadShared, ReadUnique, WriteBackFull, Evict, plus the snoop traffic
the protocol produces.

A shared ChiFinishBarrier counts completions; the 16th signal fires
sc_stop(), guaranteeing every tile runs to completion.
"""

from m5.objects import (
    ChiFinishBarrier,
    SmokeOpcodeMixDriver,
)


def build(args, planner):
    barrier = ChiFinishBarrier(expected=args.num_cpus)
    drivers = []
    lines_per_tile = 8  # per-tile stripe into one HNF slice
    for tile in range(args.num_cpus):
        # Give each tile a distinct HNF stripe so tiles touch disjoint
        # addresses. (Tile i's stripe is homed at HNF i.)
        addresses = planner.line_range(
            hnf_idx=tile, num_lines=lines_per_tile, line_offset=0
        )
        drivers.append(
            SmokeOpcodeMixDriver(
                addresses=addresses,
                access_size=8,
                iterations=args.scenario_iterations,
                percent_reads=65,
                seed=0x1000 + tile,
                finish_barrier=barrier,
            )
        )
    # The barrier becomes a child of drivers[0] via its first
    # finish_barrier Param assignment — gem5 handles the parenting
    # automatically.
    return drivers
