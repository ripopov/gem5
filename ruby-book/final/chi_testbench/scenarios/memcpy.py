# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
memcpy — Stage-2 pipelined copy using nb_transport multi-outstanding.

Tile 7 copies `num_lines` cache lines from a source range homed at
HNF 0 to a destination range homed at HNF 15. Up to `pipeline_depth`
concurrent LD/ST pairs are in flight at any moment.

Useful for sweeping bandwidth-vs-depth: `pipeline_depth` at 1 is
effectively b_transport; higher depths expose the mesh's throughput
ceiling before CHI bookkeeping or Garnet queueing saturates it.
"""

from m5.objects import (
    ChiFinishBarrier,
    IdleDriver,
    MemcpyDriver,
)


def build(args, planner):
    num_lines = 256
    src_base = planner.address_for_hnf(hnf_idx=0, line_offset=0)
    dst_base = planner.address_for_hnf(hnf_idx=15, line_offset=0)

    barrier = ChiFinishBarrier(expected=1)

    drivers = [None] * args.num_cpus
    drivers[7] = MemcpyDriver(
        src_base=src_base,
        dst_base=dst_base,
        num_lines=num_lines,
        line_size=64,
        pipeline_depth=4,
        finish_barrier=barrier,
    )
    for tile in range(args.num_cpus):
        if drivers[tile] is None:
            drivers[tile] = IdleDriver()
    return drivers
