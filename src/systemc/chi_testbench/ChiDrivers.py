# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause

from m5.objects.ClockedObject import ClockedObject
from m5.objects.SystemC import SystemC_ScModule
from m5.objects.Tlm import TlmInitiatorSocket
from m5.params import (
    NULL,
    Addr,
    Bool,
    Param,
    UInt32,
    VectorParam,
)


class ChiEventBus(SystemC_ScModule):
    """Shared SystemC sc_event rendezvous for cross-tile coordination.

    Drivers look up events by string name via the bus's `wait_on`/
    `notify` helpers; multiple drivers sharing the same bus reference
    see the same event instances.
    """

    type = "ChiEventBus"
    cxx_class = "gem5::chi_testbench::ChiEventBus"
    cxx_header = "systemc/chi_testbench/event_bus.hh"
    override_create = True


class ChiFinishBarrier(SystemC_ScModule):
    """Counter-based completion barrier.

    Drivers that hold a reference to this SimObject call
    `signal_finish()` when their run() is about to return. When the
    configured `expected` count is reached, the barrier calls
    sc_stop() to terminate the simulation.
    """

    type = "ChiFinishBarrier"
    cxx_class = "gem5::chi_testbench::ChiFinishBarrier"
    cxx_header = "systemc/chi_testbench/finish_barrier.hh"
    override_create = True

    expected = Param.UInt32(
        1, "Number of drivers that must signal before " "sc_stop is called"
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

    finish_barrier = Param.ChiFinishBarrier(
        NULL,
        "Optional completion barrier; set to share a barrier across "
        "multiple drivers and let the last finisher call sc_stop",
    )


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


class SmokeWriteDriver(ChiDriverBase):
    """Sequential blocking writes over a fixed address list."""

    type = "SmokeWriteDriver"
    cxx_class = "gem5::chi_testbench::SmokeWriteDriver"
    cxx_header = "systemc/chi_testbench/smoke_write.hh"
    override_create = True

    addresses = VectorParam.Addr([], "Addresses to write, in order")
    access_size = Param.UInt32(8, "Bytes per access")
    iterations = Param.UInt32(1, "Number of passes over the address list")
    stop_on_finish = Param.Bool(
        True, "Call sc_stop() when the sequence completes"
    )


class SmokeOpcodeMixDriver(ChiDriverBase):
    """Mix of reads and writes over a fixed address list.

    Designed to be instantiated on every tile simultaneously to exercise
    the full CHI opcode envelope (ReadShared / ReadUnique /
    WriteBackFull / Evict / snoops).
    """

    type = "SmokeOpcodeMixDriver"
    cxx_class = "gem5::chi_testbench::SmokeOpcodeMixDriver"
    cxx_header = "systemc/chi_testbench/smoke_opcode_mix.hh"
    override_create = True

    addresses = VectorParam.Addr([], "Address pool the driver shuffles across")
    access_size = Param.UInt32(8, "Bytes per access")
    iterations = Param.UInt32(1, "Number of passes over the address list")
    percent_reads = Param.UInt32(65, "0..100, remainder are writes")
    seed = Param.UInt32(0, "RNG seed (0 picks a safe default)")
    stop_on_finish = Param.Bool(
        False,
        "Call sc_stop() from this driver when its sequence completes. "
        "Enable on exactly one driver per scenario.",
    )


class IdleDriver(ChiDriverBase):
    """Generates no traffic. Silent; used at inactive tiles."""

    type = "IdleDriver"
    cxx_class = "gem5::chi_testbench::IdleDriver"
    cxx_header = "systemc/chi_testbench/idle_driver.hh"
    override_create = True


class PingPongDriver(ChiDriverBase):
    """Ownership ping-pong on one cache line between two tiles."""

    type = "PingPongDriver"
    cxx_class = "gem5::chi_testbench::PingPongDriver"
    cxx_header = "systemc/chi_testbench/ping_pong.hh"
    override_create = True

    line_addr = Param.Addr(0, "Cache-line-aligned address to ping-pong on")
    access_size = Param.UInt32(8, "Bytes per write")
    iterations = Param.UInt32(100, "Number of rounds per driver")
    initiator = Param.Bool(
        False,
        "True for the driver that writes first (the other waits first)",
    )
    wait_event = Param.String(
        "",
        "Event name this driver waits on before its write "
        "(the peer's post_event)",
    )
    post_event = Param.String(
        "",
        "Event name this driver notifies after its write "
        "(the peer's wait_event)",
    )
    bus = Param.ChiEventBus(NULL, "Shared sc_event rendezvous")


class FalseSharingDriver(ChiDriverBase):
    """Writes one byte at a fixed offset of a shared cache line."""

    type = "FalseSharingDriver"
    cxx_class = "gem5::chi_testbench::FalseSharingDriver"
    cxx_header = "systemc/chi_testbench/false_sharing.hh"
    override_create = True

    line_addr = Param.Addr(0, "Cache-line-aligned address all sharers hit")
    byte_offset = Param.UInt32(0, "Byte offset within the line (0..63)")
    iterations = Param.UInt32(1000, "Number of writes")


class MemcpyDriver(ChiDriverBase):
    """Pipelined memcpy using nb_transport multi-outstanding requests."""

    type = "MemcpyDriver"
    cxx_class = "gem5::chi_testbench::MemcpyDriver"
    cxx_header = "systemc/chi_testbench/memcpy_driver.hh"
    override_create = True

    src_base = Param.Addr(0, "Base address of the source range")
    dst_base = Param.Addr(0, "Base address of the destination range")
    num_lines = Param.UInt32(256, "Number of cache lines to copy")
    line_size = Param.UInt32(64, "Bytes per line")
    pipeline_depth = Param.UInt32(
        4, "Number of concurrent outstanding LD+ST slots"
    )


class MemsetDriver(ChiDriverBase):
    """Pipelined memset using nb_transport multi-outstanding requests."""

    type = "MemsetDriver"
    cxx_class = "gem5::chi_testbench::MemsetDriver"
    cxx_header = "systemc/chi_testbench/memset_driver.hh"
    override_create = True

    dst_base = Param.Addr(0, "Base address of the range to fill")
    num_lines = Param.UInt32(256, "Number of cache lines to write")
    line_size = Param.UInt32(64, "Bytes per line")
    pipeline_depth = Param.UInt32(
        4, "Number of concurrent outstanding store slots"
    )
    fill_byte = Param.UInt32(
        0xAA, "Byte value to write into every destination line"
    )
