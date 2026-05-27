# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause

from m5.params import *
from m5.SimObject import SimObject


class ChiSequence(SimObject):
    """Abstract base for CHI testbench sequences.

    The driver (ChiSeqDriver) holds a Param.ChiSequence and invokes
    `run(driver)` from its fiber. Each concrete subclass declares only
    the parameters it actually uses, restoring Open/Closed: adding a
    new sequence means adding one Python subclass + one C++ pair under
    `sequences/`, with no edit to the driver.
    """

    type = "ChiSequence"
    abstract = True
    cxx_class = "gem5::chi_gem5tb::ChiSequence"
    cxx_header = "chi_testbench_gem5/sequences/base.hh"


class PingPongSequence(ChiSequence):
    type = "PingPongSequence"
    cxx_class = "gem5::chi_gem5tb::PingPongSequence"
    cxx_header = "chi_testbench_gem5/sequences/ping_pong.hh"

    line_addr = Param.Addr(0, "Cache-line address being ping-ponged")
    iterations = Param.UInt32(1, "Iteration count")
    initiator = Param.Bool(False, "True for the tile that writes first")
    l3_clock = Param.Clock("1GHz", "L3/Ruby clock period")
    roi_iteration = Param.UInt32(0, "Measured iteration to mark as ROI")
    dump_roi_stats = Param.Bool(False, "Dump stats for the marked ROI")
    full_line_writes = Param.Bool(
        False, "Write the full cache line when handing off the turn"
    )
    line_size = Param.UInt32(64, "Cache line size for full-line writes")


class MemsetSequence(ChiSequence):
    type = "MemsetSequence"
    cxx_class = "gem5::chi_gem5tb::MemsetSequence"
    cxx_header = "chi_testbench_gem5/sequences/memset.hh"

    dst_base = Param.Addr(0, "Destination base address")
    num_lines = Param.UInt32(0, "Number of cache lines to fill")
    line_size = Param.UInt32(64, "Bytes per cache line")
    write_size = Param.UInt32(
        0, "Bytes written per cache line; 0 means line_size"
    )
    num_outstanding_reqs = Param.UInt32(
        4, "Maximum in-flight requests issued by this sequence"
    )
    pipeline_depth = Param.UInt32(
        0, "Deprecated alias for num_outstanding_reqs"
    )
    fill_byte = Param.UInt32(0xAA, "Byte value to fill")
    warmup_l3 = Param.Bool(
        False,
        "First fill the range with full-line stores to warm L3 only",
    )
    roi_stats = Param.Bool(False, "Reset/dump stats around the 80% ROI")
    roi_participants = Param.UInt32(
        1, "Number of memset drivers participating in ROI synchronization"
    )
