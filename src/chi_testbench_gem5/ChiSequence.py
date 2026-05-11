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


class IdleSequence(ChiSequence):
    """Default body — no traffic."""

    type = "IdleSequence"
    cxx_class = "gem5::chi_gem5tb::IdleSequence"
    cxx_header = "chi_testbench_gem5/sequences/idle.hh"


class SmokeReadSequence(ChiSequence):
    type = "SmokeReadSequence"
    cxx_class = "gem5::chi_gem5tb::SmokeReadSequence"
    cxx_header = "chi_testbench_gem5/sequences/smoke_read.hh"

    addresses = VectorParam.Addr([], "Addresses to read")
    access_size = Param.UInt32(8, "Bytes per access")
    iterations = Param.UInt32(1, "Iteration count")


class SmokeWriteSequence(ChiSequence):
    type = "SmokeWriteSequence"
    cxx_class = "gem5::chi_gem5tb::SmokeWriteSequence"
    cxx_header = "chi_testbench_gem5/sequences/smoke_write.hh"

    addresses = VectorParam.Addr([], "Addresses to write")
    access_size = Param.UInt32(8, "Bytes per access")
    iterations = Param.UInt32(1, "Iteration count")


class SmokeOpcodeMixSequence(ChiSequence):
    type = "SmokeOpcodeMixSequence"
    cxx_class = "gem5::chi_gem5tb::SmokeOpcodeMixSequence"
    cxx_header = "chi_testbench_gem5/sequences/smoke_opcode_mix.hh"

    addresses = VectorParam.Addr([], "Addresses to access")
    access_size = Param.UInt32(8, "Bytes per access")
    iterations = Param.UInt32(1, "Iteration count")
    percent_reads = Param.UInt32(65, "Read fraction (0..100)")
    seed = Param.UInt32(0, "RNG seed (0 = default)")


class PingPongSequence(ChiSequence):
    type = "PingPongSequence"
    cxx_class = "gem5::chi_gem5tb::PingPongSequence"
    cxx_header = "chi_testbench_gem5/sequences/ping_pong.hh"

    line_addr = Param.Addr(0, "Cache-line address being ping-ponged")
    iterations = Param.UInt32(1, "Iteration count")
    initiator = Param.Bool(False, "True for the tile that writes first")
    l3_clock = Param.Clock("1GHz", "L3/Ruby clock period")


class FalseSharingSequence(ChiSequence):
    type = "FalseSharingSequence"
    cxx_class = "gem5::chi_gem5tb::FalseSharingSequence"
    cxx_header = "chi_testbench_gem5/sequences/false_sharing.hh"

    line_addr = Param.Addr(0, "Cache-line address shared by both tiles")
    byte_offset = Param.UInt32(0, "Byte offset within the line for this tile")
    iterations = Param.UInt32(1, "Iteration count")


class MemcpySequence(ChiSequence):
    type = "MemcpySequence"
    cxx_class = "gem5::chi_gem5tb::MemcpySequence"
    cxx_header = "chi_testbench_gem5/sequences/memcpy.hh"

    src_base = Param.Addr(0, "Source base address")
    dst_base = Param.Addr(0, "Destination base address")
    num_lines = Param.UInt32(0, "Number of cache lines to copy")
    line_size = Param.UInt32(64, "Bytes per cache line")
    pipeline_depth = Param.UInt32(4, "In-flight read+write pair cap")


class MemsetSequence(ChiSequence):
    type = "MemsetSequence"
    cxx_class = "gem5::chi_gem5tb::MemsetSequence"
    cxx_header = "chi_testbench_gem5/sequences/memset.hh"

    dst_base = Param.Addr(0, "Destination base address")
    num_lines = Param.UInt32(0, "Number of cache lines to fill")
    line_size = Param.UInt32(64, "Bytes per cache line")
    pipeline_depth = Param.UInt32(4, "In-flight write cap")
    fill_byte = Param.UInt32(0xAA, "Byte value to fill")


class OpcodeWalkSequence(ChiSequence):
    type = "OpcodeWalkSequence"
    cxx_class = "gem5::chi_gem5tb::OpcodeWalkSequence"
    cxx_header = "chi_testbench_gem5/sequences/opcode_walk.hh"

    target_addr = Param.Addr(0, "Target line walked through opcode states")
    filler_base = Param.Addr(0, "Base of capacity-eviction filler stripe")
    filler_lines = Param.UInt32(
        0, "Number of filler reads (capacity pressure)"
    )


class ReadExWalkSequence(ChiSequence):
    type = "ReadExWalkSequence"
    cxx_class = "gem5::chi_gem5tb::ReadExWalkSequence"
    cxx_header = "chi_testbench_gem5/sequences/read_ex_walk.hh"

    target_addr = Param.Addr(0, "Shared target line address")
    access_size = Param.UInt32(64, "Bytes per access")
    role = Param.String("", "Role label (A, B, or C)")
