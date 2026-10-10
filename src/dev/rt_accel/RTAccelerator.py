# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.objects.AbstractMemory import AbstractMemory
from m5.objects.ClockedObject import ClockedObject
from m5.objects.System import System
from m5.params import *
from m5.proxy import Parent
from m5.SimObject import SimObject


class RTAccelHostAdapter(SimObject):
    type = "RTAccelHostAdapter"
    cxx_header = "dev/rt_accel/host_adapter.hh"
    cxx_class = "gem5::rt_accel::HostAdapter"

    system = Param.System(Parent.any, "Host memory system")
    trace_file = Param.String("", "Optional transaction trace")
    mmio_range = Param.AddrRange("Accelerator MMIO window")
    queue_depth = Param.Unsigned(16, "Maximum outstanding MMIO requests")
    pio = ResponsePort("Host MMIO requests and responses")
    dma = RequestPort("Noncoherent DMA and MSI requests")


class RTAccelerator(ClockedObject):
    type = "RTAccelerator"
    cxx_header = "dev/rt_accel/accelerator_wrapper.hh"
    cxx_class = "gem5::rt_accel::AcceleratorWrapper"

    host = Param.RTAccelHostAdapter("Host adapter on the host event queue")
    queue_depth = Param.Unsigned(16, "Model and master request capacity")
    request_delay = Param.Latency("5ns", "Transport request delay")
    response_delay = Param.Latency("5ns", "Transport response delay")


class RTGem5Accelerator(ClockedObject):
    type = "RTGem5Accelerator"
    cxx_header = "dev/rt_accel/gem5_wrapper.hh"
    cxx_class = "gem5::rt_accel::Gem5Wrapper"

    trace_file = Param.String("", "Optional transaction trace")
    host = Param.RTAccelHostAdapter("Host adapter")
    index = Param.Unsigned("Deterministic transport arbitration index")
    queue_depth = Param.Unsigned(16, "Outstanding master credits")
    request_delay = Param.Latency("5ns", "Request transport stage")
    response_delay = Param.Latency("5ns", "Response transport stage")
    system_ranges = VectorParam.AddrRange(
        "Host system memory and MSI apertures"
    )
    mmio = RequestPort("Incoming host MMIO to the local crossbar")
    memory = ResponsePort("Local crossbar system-memory egress")


class RTAccelControl(ClockedObject):
    type = "RTAccelControl"
    cxx_header = "dev/rt_accel/control.hh"
    cxx_class = "gem5::rt_accel::Control"

    system = Param.System(Parent.any, "Local accelerator system")
    cpu = Param.BaseCPU("Single accelerator hart")
    registers = Param.AddrRange("Control register aperture")
    data_tcm_range = Param.AddrRange("DMA-accessible writable Data TCM")
    host_range = Param.AddrRange("DMA-accessible host RAM")
    max_outstanding = Param.Unsigned(256, "Maximum live DMA chunks")
    coherent_dma = Param.Bool(False, "Snoop and invalidate cached Data TCM")
    pio = ResponsePort("Control registers")
    dma = RequestPort("Pipelined DMA reads via local crossbar")
    dma_write = RequestPort("Pipelined DMA writes and ordered MSI")
    dma_clean = RequestPort("Optional Data TCM coherence preparation")


class RTAccelCodeTCM(AbstractMemory):
    type = "RTAccelCodeTCM"
    cxx_header = "dev/rt_accel/code_tcm.hh"
    cxx_class = "gem5::rt_accel::CodeTCM"

    writeable = False
    latency = Param.Latency("1ns", "Pipelined read response latency")
    instruction = ResponsePort("CPU instruction fetches through optional L1I")
    constants = ResponsePort("Constant reads and functional ELF loading")


class RTAccelMSI(ClockedObject):
    type = "RTAccelMSI"
    cxx_header = "dev/rt_accel/msi.hh"
    cxx_class = "gem5::rt_accel::MSITarget"

    trace_file = Param.String("", "Optional completion visibility trace")
    cpu = Param.BaseCPU("Host hart receiving machine external interrupts")
    accelerator_cores = Param.Unsigned(8, "Discoverable core count (1-32)")
    registers = Param.AddrRange("MSI target and acknowledgement aperture")
    pio = ResponsePort("MSI writes, pending bitmap, and acknowledgement")
