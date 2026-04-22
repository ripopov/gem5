# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
smoke_opcode_mix — One ChiSeqDriver per tile runs the smoke_opcode_mix
sequence concurrently over its own private stripe. Exercises the full
demand opcode envelope (ReadShared, ReadUnique, WriteBackFull, Evict,
SnpUnique) across all 16 tiles simultaneously. The 16th
signal_finish() on the shared barrier terminates the sim.
"""

from m5.objects import (
    ChiGem5Barrier,
    ChiSeqDriver,
    SmokeOpcodeMixSequence,
)


def build(args, planner):
    barrier = ChiGem5Barrier(expected=args.num_cpus)
    lines_per_tile = 8

    drivers = []
    for tile in range(args.num_cpus):
        # Each tile owns the stripe homed at its own HNF index.
        addresses = planner.line_range(
            hnf_idx=tile, num_lines=lines_per_tile, line_offset=0
        )
        drivers.append(
            ChiSeqDriver(
                tile_id=tile,
                finish_barrier=barrier,
                sequence=SmokeOpcodeMixSequence(
                    addresses=addresses,
                    access_size=8,
                    iterations=args.scenario_iterations,
                    percent_reads=65,
                    seed=0x1000 + tile,
                ),
            )
        )
    return drivers
