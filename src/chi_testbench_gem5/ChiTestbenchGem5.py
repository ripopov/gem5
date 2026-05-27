# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause

from m5.objects.ChiSequence import (
    ChiSequence,
    MemsetSequence,
)
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
    """Generic per-tile driver. The body of work is a polymorphic
    `ChiSequence` SimObject — see ChiSequence.py for the concrete
    subclasses. Adding a new sequence does not require touching this
    class.

    ChiSeqDriver is a ClockedObject that sits at system.cpu[i]. It
    owns a RequestPort (`port`) wired to the RN-F sequencer's
    in_ports, and a Fiber on which the selected sequence runs.
    Blocking helpers (read/write/read_exclusive) suspend the fiber
    until the CHI response arrives; non-blocking helpers submit
    requests and queue responses for ordered sequence-side polling.
    """

    type = "ChiSeqDriver"
    cxx_class = "gem5::chi_gem5tb::ChiSeqDriver"
    cxx_header = "chi_testbench_gem5/driver.hh"

    port = RequestPort("Data port into the RN-F sequencer")
    system = Param.System(Parent.any, "System this driver is part of")

    sequence = Param.ChiSequence(
        MemsetSequence(num_lines=0), "Sequence body to run on this tile"
    )
    tile_id = Param.UInt32(0, "Logical tile id")

    finish_barrier = Param.ChiGem5Barrier(NULL, "Completion barrier")
    event_bus = Param.ChiGem5EventBus(NULL, "Cross-tile latch registry")
