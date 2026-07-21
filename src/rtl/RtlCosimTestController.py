# Copyright (c) 2026 The gem5 Authors
# SPDX-License-Identifier: BSD-3-Clause

from m5.objects.ClockedObject import ClockedObject
from m5.params import (
    Param,
    VectorIntSourcePin,
    VectorParam,
    VectorResetRequestPort,
)


class RtlCosimTestController(ClockedObject):
    """Drive validation events and stop after every RTL core is idle."""

    type = "RtlCosimTestController"
    cxx_header = "rtl/test_controller.hh"
    cxx_class = "gem5::rtl_cosim::RtlCosimTestController"

    cores = VectorParam.RtlCoreSimObject([], "RTL cores to monitor")
    mode = Param.String("idle", "idle, interrupt, or reset")
    poll_interval = Param.Cycles(10, "Idle polling interval")
    pulse_cycles = Param.Cycles(5, "Interrupt or reset pulse duration")
    timeout_cycles = Param.Cycles(500000, "Validation timeout")

    interrupt_outputs = VectorIntSourcePin("Validation interrupt sources")
    reset_outputs = VectorResetRequestPort("Validation reset sources")
