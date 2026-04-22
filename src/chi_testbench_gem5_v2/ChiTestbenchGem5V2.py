# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause

from m5.objects.CHIGeneric import CHIGenericController
from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject


class ChiGem5V2Barrier(SimObject):
    """Counter-based completion barrier.

    When `expected` drivers have called `signal_finish()`, `exitSimLoop()`
    fires. The `Gem5V2` infix avoids symbol collisions with v1's
    `ChiGem5Barrier` when both testbenches are linked into the same
    gem5 binary.
    """

    type = "ChiGem5V2Barrier"
    cxx_class = "gem5::chi_gem5tb_v2::ChiBarrier"
    cxx_header = "chi_testbench_gem5_v2/sync/barrier.hh"

    expected = Param.UInt32(
        1, "Number of participants that must signal before exit"
    )


class ChiGem5V2EventBus(SimObject):
    """Named-latch registry for cross-tile rendezvous.

    Drivers sharing a bus call `wait_on(name)` / `notify(name)` with the
    same string key to synchronize their sequences.
    """

    type = "ChiGem5V2EventBus"
    cxx_class = "gem5::chi_gem5tb_v2::ChiEventBus"
    cxx_header = "chi_testbench_gem5_v2/sync/latch.hh"


class ChiDriverNode(CHIGenericController):
    """Synthetic upstream CHI requester, one per tile.

    ChiDriverNode is a CHIGenericController subclass, which means it is
    a real Ruby network node (its eight CHI MessageBuffers are wired to
    Garnet the same way any SLICC controller's are), but its behavior
    is driven by Fiber-hosted C++ sequences rather than by a SLICC
    automaton. Sequences select their body via the `sequence` string;
    the driver constructs `CHIRequestMsg` / `CHIResponseMsg` /
    `CHIDataMsg` directly and enqueues them on the outbound buffers.

    The driver's `downstream_destinations` should list the MachineID of
    the colocated tile cache controller, which is the default target of
    `read_shared(addr)` and friends.
    """

    type = "ChiDriverNode"
    cxx_class = "gem5::chi_gem5tb_v2::ChiDriverNode"
    cxx_header = "chi_testbench_gem5_v2/driver.hh"

    sequence = Param.String("idle", "Registered sequence name")
    tile_id = Param.UInt32(0, "Logical tile id")

    # Sequence parameters (all optional, defaulted — each sequence
    # picks out only the fields it needs).
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

    role = Param.String("", "Role label within a multi-tile scenario")

    finish_barrier = Param.ChiGem5V2Barrier(NULL, "Completion barrier")
    event_bus = Param.ChiGem5V2EventBus(NULL, "Cross-tile latch registry")
