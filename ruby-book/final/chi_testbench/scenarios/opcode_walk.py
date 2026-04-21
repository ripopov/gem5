# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
opcode_walk — Stage-2 data-dependent scripted walk.

Tile 3 runs a 4-step sequence:
  1. cold ReadShared of the target line homed at HNF 3
  2. WriteUnique of a 0xA5 pattern, then hot readback (assertion)
  3. filler-loop capacity pressure to force WriteBackFull + eviction
  4. cold-ish read of the target (assertion: data still matches pattern)

A failed assertion triggers panic() from the SC_THREAD, which surfaces
as a simulation abort — the test's own failure signal.

What to check in stats.txt:
- Per-HNF demand-accesses / demand-misses consistent with 2+ line
  touches on HNF 3 and thousands on the HNF stripes the filler walks.
- WriteBackFull transitions appear in the HNF 3 controller after step 3.
"""

from m5.objects import (
    ChiFinishBarrier,
    IdleDriver,
    OpcodeWalkDriver,
)


def build(args, planner):
    target_addr = planner.address_for_hnf(hnf_idx=3, line_offset=0)
    # Filler range that sweeps all HNFs, ~128 KiB of distinct lines,
    # enough to evict any single target from a typical L1+L2 tile.
    filler_base = planner.address_for_hnf(hnf_idx=0, line_offset=16)
    barrier = ChiFinishBarrier(expected=1)

    drivers = [None] * args.num_cpus
    drivers[3] = OpcodeWalkDriver(
        target_addr=target_addr,
        filler_base=filler_base,
        filler_lines=2048,
        finish_barrier=barrier,
    )
    for tile in range(args.num_cpus):
        if drivers[tile] is None:
            drivers[tile] = IdleDriver()
    return drivers
