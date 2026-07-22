# Copyright (c) 2026 The gem5 Authors
# SPDX-License-Identifier: BSD-3-Clause

from m5.objects.ClockedObject import ClockedObject
from m5.objects.BaseCPU import BaseCPU
from m5.defines import buildEnv
from m5.objects.SignalPort import (
    VectorSignalSinkPort,
    VectorSignalSourcePort,
)
from m5.params import (
    Param,
    VectorIntSinkPin,
    VectorIntSourcePin,
    VectorParam,
    VectorRequestPort,
    VectorResetRequestPort,
    VectorResetResponsePort,
    VectorResponsePort,
)
from m5.proxy import Parent


# BaseCPU's Python helpers require architecture traits. Select the build ISA,
# while keeping the C++ switch implementation and vendor ABI ISA-neutral.
if buildEnv.get("USE_RISCV_ISA", False):
    from m5.objects.RiscvDecoder import RiscvDecoder as _RtlDecoder
    from m5.objects.RiscvInterrupts import RiscvInterrupts as _RtlInterrupts
    from m5.objects.RiscvISA import RiscvISA as _RtlISA
    from m5.objects.RiscvMMU import RiscvMMU as _RtlMMU
elif buildEnv.get("USE_ARM_ISA", False):
    from m5.objects.ArmDecoder import ArmDecoder as _RtlDecoder
    from m5.objects.ArmInterrupts import ArmInterrupts as _RtlInterrupts
    from m5.objects.ArmISA import ArmISA as _RtlISA
    from m5.objects.ArmMMU import ArmMMU as _RtlMMU
elif buildEnv.get("USE_X86_ISA", False):
    from m5.objects.X86Decoder import X86Decoder as _RtlDecoder
    from m5.objects.X86ISA import X86ISA as _RtlISA
    from m5.objects.X86LocalApic import X86LocalApic as _RtlInterrupts
    from m5.objects.X86MMU import X86MMU as _RtlMMU
else:
    _RtlDecoder = _RtlInterrupts = _RtlISA = _RtlMMU = None


RtlSignalSinkPort = VectorSignalSinkPort("gem5::rtl_cosim::SignalValue")
RtlSignalSourcePort = VectorSignalSourcePort(
    "gem5::rtl_cosim::SignalValue"
)


class RtlCoreSimObject(ClockedObject):
    type = "RtlCoreSimObject"
    cxx_header = "rtl/rtl_core.hh"
    cxx_class = "gem5::rtl_cosim::RtlCoreSimObject"

    system = Param.System(Parent.any, "System containing this RTL core")
    library = Param.String("Vendor RTL shared-library path")
    model_config = Param.String("{}", "JSON passed to createCore")
    model_config_file = Param.String("", "Optional file containing model JSON")

    initiator_ports = VectorRequestPort("RTL initiator buses")
    target_ports = VectorResponsePort("RTL target buses")
    interrupt_inputs = VectorIntSinkPin("Interrupts driven into RTL")
    interrupt_outputs = VectorIntSourcePin("Interrupts driven by RTL")
    reset_inputs = VectorResetResponsePort("Reset inputs driven into RTL")
    reset_outputs = VectorResetRequestPort("Reset requests driven by RTL")
    io_inputs = RtlSignalSinkPort("Standalone scalar/vector RTL inputs")
    io_outputs = RtlSignalSourcePort("Standalone scalar/vector RTL outputs")

    initiator_bus_names = VectorParam.String(
        [], "API bus names in initiator_ports vector order"
    )
    target_bus_names = VectorParam.String(
        [], "API bus names in target_ports vector order"
    )
    target_addr_ranges = VectorParam.AddrRange(
        [], "Address range exposed by each RTL target bus"
    )
    interrupt_input_names = VectorParam.String(
        [], "API signal names in interrupt_inputs vector order"
    )
    interrupt_output_names = VectorParam.String(
        [], "API signal names in interrupt_outputs vector order"
    )
    reset_input_names = VectorParam.String(
        [], "API signal names in reset_inputs vector order"
    )
    reset_output_names = VectorParam.String(
        [], "API signal names in reset_outputs vector order"
    )
    io_input_names = VectorParam.String(
        [], "API signal names in io_inputs vector order"
    )
    io_input_values = VectorParam.String(
        [], "Initial integer value for each unconnected io_inputs element"
    )
    io_output_names = VectorParam.String(
        [], "API signal names in io_outputs vector order"
    )

    initial_reset_cycles = Param.Cycles(
        10, "Cycles for which all mapped reset inputs start asserted"
    )
    max_pending_transactions = Param.Unsigned(
        64, "Maximum neutral transactions pending per bus"
    )
    error_ranges = VectorParam.AddrRange(
        [], "Optional initiator ranges completed locally with an error"
    )

    image = Param.String("", "Optional ELF, COFF, or raw image")
    image_format = Param.String("auto", "auto, elf, coff, or raw")
    raw_image_address = Param.Addr(0, "Load address for a raw image")
    image_bus = Param.String(
        "", "Initiator bus used for non-TCM functional image writes"
    )
    defer_startup = Param.Bool(
        False,
        "Hold reset until a parent RtlCpuSimObject imports CPU state",
    )


class RtlCpuSimObject(BaseCPU):
    type = "RtlCpuSimObject"
    cxx_header = "rtl/rtl_cpu.hh"
    cxx_class = "gem5::rtl_cosim::RtlCpuSimObject"

    if _RtlMMU is not None:
        ArchDecoder = _RtlDecoder
        ArchInterrupts = _RtlInterrupts
        ArchISA = _RtlISA
        ArchMMU = _RtlMMU
        mmu = _RtlMMU()

    rtl_core = Param.RtlCoreSimObject(
        "Preconnected deferred RTL runtime containing the CPU model"
    )

    def __init__(self, **kwargs):
        if _RtlMMU is None:
            raise RuntimeError(
                "RtlCpuSimObject has no architecture traits in this build"
            )
        super().__init__(**kwargs)

    @classmethod
    def memory_mode(cls):
        return "timing"

    @classmethod
    def support_take_over(cls):
        return True

    @classmethod
    def support_switch_out(cls):
        return False
