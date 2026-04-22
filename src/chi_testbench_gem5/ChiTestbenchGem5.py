# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause

from m5.objects.ClockedObject import ClockedObject
from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject


class ChiGem5Barrier(SimObject):
    """Counter-based completion barrier.

    When the configured number of participants call `signal_finish()`,
    `exitSimLoop()` is invoked to terminate the simulation. The `Gem5`
    infix in the SimObject name avoids a collision with `ChiBarrier`
    elsewhere in the tree when both libraries are linked into the
    same binary.
    """

    type = "ChiGem5Barrier"
    cxx_class = "gem5::chi_gem5tb::ChiBarrier"
    cxx_header = "chi_testbench_gem5/sync/barrier.hh"

    expected = Param.UInt32(
        1, "Number of participants that must signal before exit"
    )


class ChiGem5EventBus(SimObject):
    """Named-latch registry for cross-tile rendezvous.

    Drivers call `wait_on(name)` / `notify(name)` with string keys;
    multiple drivers sharing the same bus instance see the same
    latches.
    """

    type = "ChiGem5EventBus"
    cxx_class = "gem5::chi_gem5tb::ChiEventBus"
    cxx_header = "chi_testbench_gem5/sync/latch.hh"


class ChiSeqDriver(ClockedObject):
    """Single parameterized driver. `sequence` picks a registered body.

    ChiSeqDriver is a ClockedObject that sits at system.cpu[i]. It owns
    a RequestPort (`port`) wired to the RN-F sequencer's in_ports, and
    a Fiber on which the selected sequence runs. Blocking helpers
    (read/write/read_exclusive) suspend the fiber until the CHI
    response arrives; non-blocking helpers (async_*) let the sequence
    keep many requests in flight.
    """

    type = "ChiSeqDriver"
    cxx_class = "gem5::chi_gem5tb::ChiSeqDriver"
    cxx_header = "chi_testbench_gem5/driver.hh"

    port = RequestPort("Data port into the RN-F sequencer")
    system = Param.System(Parent.any, "System this driver is part of")

    sequence = Param.String("idle", "Registered sequence name")
    tile_id = Param.UInt32(0, "Logical tile id")

    # Sequence parameters (all optional, defaulted — each sequence
    # picks out only the fields it needs). Intentionally flat to keep
    # Python wiring simple.
    addresses = VectorParam.Addr([], "Address list")
    access_size = Param.UInt32(8, "Bytes per access")
    iterations = Param.UInt32(1, "Iteration count")

    target_addr = Param.Addr(0, "Primary target address")
    filler_base = Param.Addr(0, "Filler stripe base")
    filler_lines = Param.UInt32(0, "Filler line count")

    line_addr = Param.Addr(0, "Cache-line address for ping_pong/false_sharing")
    byte_offset = Param.UInt32(0, "Byte offset within a line")

    pipeline_depth = Param.UInt32(4, "Async in-flight cap")
    num_lines = Param.UInt32(0, "Line count for memcpy/memset")
    line_size = Param.UInt32(64, "Bytes per line")
    src_base = Param.Addr(0, "memcpy source base")
    dst_base = Param.Addr(0, "memcpy/memset destination base")
    fill_byte = Param.UInt32(0xAA, "memset fill byte")

    initiator = Param.Bool(False, "ping_pong initiator flag")
    percent_reads = Param.UInt32(65, "smoke_opcode_mix read fraction")
    seed = Param.UInt32(0, "smoke_opcode_mix RNG seed (0 = default)")

    wait_event_name = Param.String("", "Event name to wait on")
    post_event_name = Param.String("", "Event name to notify after write")

    # A / B / C are generic 3-tile rendezvous labels used by
    # read_ex_walk. Each driver looks up latches in the shared bus by
    # these names.
    role = Param.String("", "Role label within a multi-tile scenario")

    finish_barrier = Param.ChiGem5Barrier(NULL, "Completion barrier")
    event_bus = Param.ChiGem5EventBus(NULL, "Cross-tile latch registry")
