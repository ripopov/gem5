from m5.objects.BaseNonCachingSimpleCPU import BaseNonCachingSimpleCPU
from m5.objects.RiscvDecoder import RiscvDecoder
from m5.objects.RiscvInterrupts import RiscvInterrupts
from m5.objects.RiscvISA import RiscvISA
from m5.objects.RiscvMMU import RiscvMMU
from m5.params import Param, Unsigned


class RiscvJitCPU(BaseNonCachingSimpleCPU):
    type = "RiscvJitCPU"
    cxx_header = "cpu/jit/riscv_jit_cpu.hh"
    cxx_class = "gem5::RiscvJitCPU"

    ArchDecoder = RiscvDecoder
    ArchMMU = RiscvMMU
    ArchInterrupts = RiscvInterrupts
    ArchISA = RiscvISA

    mmu = RiscvMMU()
    numThreads = 1

    backend_path = Param.String(
        "libgem5-qemu-jit.so",
        "Path to the QEMU/TCG JitCPU backend shared library",
    )
    batch_size = Param.Unsigned(
        10000,
        "Maximum translated guest instructions executed per gem5 event",
    )
