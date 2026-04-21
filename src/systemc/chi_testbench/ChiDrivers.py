# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause

from m5.objects.ClockedObject import ClockedObject
from m5.objects.SystemC import SystemC_ScModule
from m5.objects.Tlm import TlmInitiatorSocket
from m5.params import (
    Addr,
    Bool,
    Param,
    UInt32,
    VectorParam,
)


class ChiTileSlot(ClockedObject):
    """Placeholder SimObject for system.cpu[i] when no real CPU is used.

    CHI_RNF attaches RubySequencer / L1 / L2 children to each cpu slot;
    those children need a proper gem5 SimObject parent so their stat
    groups register under a unique path. The SystemC-side driver is
    mounted separately (system.drivers[i]) and wired through a TLM
    bridge to the sequencer's in_ports.
    """

    type = "ChiTileSlot"
    cxx_class = "gem5::chi_testbench::ChiTileSlot"
    cxx_header = "systemc/chi_testbench/tile_slot.hh"


class ChiDriverBase(SystemC_ScModule):
    """Abstract base for every SystemC CHI test driver."""

    type = "ChiDriverBase"
    abstract = True
    cxx_class = "gem5::chi_testbench::ChiDriverBase"
    cxx_header = "systemc/chi_testbench/driver_base.hh"

    iSocket = TlmInitiatorSocket(64, "TLM initiator into the CHI mesh")


class SmokeReadDriver(ChiDriverBase):
    """Sequential blocking reads over a fixed address list."""

    type = "SmokeReadDriver"
    cxx_class = "gem5::chi_testbench::SmokeReadDriver"
    cxx_header = "systemc/chi_testbench/smoke_read.hh"
    override_create = True

    addresses = VectorParam.Addr([], "Addresses to read, in order")
    access_size = Param.UInt32(8, "Bytes per access")
    iterations = Param.UInt32(1, "Number of passes over the address list")
    stop_on_finish = Param.Bool(
        True, "Call sc_stop() when the sequence completes"
    )
