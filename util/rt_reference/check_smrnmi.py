# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
"""Execute positive and negative Smrnmi capability checks on a bare hart.

Use low-level wiring to isolate CSR permission and reset state without a board
or operating system. This is a focused check of the cached ISA capability.
"""

import argparse
from pathlib import Path

import m5
from m5.objects import (
    AddrRange,
    NoncoherentXBar,
    RiscvBareMetal,
    RiscvISA,
    RiscvSystem,
    RiscvTimingSimpleCPU,
    Root,
    SimpleMemory,
    SrcClockDomain,
    VoltageDomain,
)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--enabled", action="store_true")
args = parser.parse_args()
firmware = (
    Path(__file__).resolve().parent
    / "out"
    / ("smrnmi-present.elf" if args.enabled else "smrnmi-absent.elf")
)
system = RiscvSystem(mem_mode="timing")
system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=system.voltage_domain
)
system.workload = RiscvBareMetal(bootloader=str(firmware))
system.cpu = RiscvTimingSimpleCPU(numThreads=1)
system.cpu.isa = [
    RiscvISA(
        riscv_profile="RVI20U64",
        extra_extensions=["M", "A", "C", "Zicsr"]
        + (["Smrnmi"] if args.enabled else []),
    )
]
system.cpu.createThreads()
system.cpu.createInterruptController()
system.bus = NoncoherentXBar(
    frontend_latency=1, forward_latency=1, response_latency=1, width=32
)
system.cpu.icache_port = system.bus.cpu_side_ports
system.cpu.dcache_port = system.bus.cpu_side_ports
system.cpu.mmu.connectWalkerPorts(
    system.bus.cpu_side_ports, system.bus.cpu_side_ports
)
system.mem_ranges = [AddrRange(0x80000000, size="64KiB")]
system.memory = SimpleMemory(range=system.mem_ranges[0], latency="1ns")
system.memory.port = system.bus.mem_side_ports
system.memories = [system.memory]
system.system_port = system.bus.cpu_side_ports
root = Root(full_system=True, system=system)
m5.instantiate()
event = m5.simulate(1000000)
assert event.getCause() == "m5_exit instruction encountered", (
    event.getCause(),
    event.getCode(),
)
assert event.getCode() == 0
print(f"PASS: Smrnmi {'present' if args.enabled else 'absent'}")
