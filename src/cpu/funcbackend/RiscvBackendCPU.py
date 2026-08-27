# Copyright (c) 2026 Roman Popov
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


from m5.objects.BaseNonCachingSimpleCPU import BaseNonCachingSimpleCPU
from m5.objects.RiscvDecoder import RiscvDecoder
from m5.objects.RiscvInterrupts import RiscvInterrupts
from m5.objects.RiscvISA import RiscvISA
from m5.objects.RiscvMMU import RiscvMMU
from m5.params import Param


class RiscvBackendCPU(BaseNonCachingSimpleCPU):
    """A functional RV64 CPU whose execution happens outside gem5.

    Subclasses name one external engine each; everything that makes such an
    engine behave like a gem5 CPU is shared.
    """

    type = "RiscvBackendCPU"
    abstract = True
    cxx_header = "cpu/funcbackend/riscv_backend_cpu.hh"
    cxx_class = "gem5::RiscvBackendCPU"

    ArchDecoder = RiscvDecoder
    ArchMMU = RiscvMMU
    ArchInterrupts = RiscvInterrupts
    ArchISA = RiscvISA

    isa = [
        RiscvISA(
            riscv_profile="RVI20U64",
            extra_extensions=[
                "M",
                "A",
                "F",
                "D",
                "C",
                "Zicntr",
                "Zicsr",
                "Zifencei",
                "Zihpm",
                "Zba",
                "Zbb",
                "Zbs",
                "Sv39",
                "Svnapot",
            ],
        )
    ]
    mmu = RiscvMMU()
    numThreads = 1

    backend_path = Param.String(
        "Path to the backend shared library",
    )
    backend_instance = Param.Unsigned(
        0,
        "This CPU's instance ID within the shared backend library",
    )
    backend_instance_count = Param.Unsigned(
        1,
        "Number of CPUs sharing the backend library",
    )
    batch_size = Param.Unsigned(
        10000,
        "Maximum guest instructions executed per gem5 event",
    )
