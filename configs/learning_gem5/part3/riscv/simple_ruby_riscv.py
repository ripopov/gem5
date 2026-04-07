"""RISC-V equivalent of simple_ruby.py — runs the 'threads' binary on two
RiscvTimingSimpleCPU cores with the MSI Ruby protocol.

Reuses MyCacheSystem from the parent directory's msi_caches.py.
"""

import os
import sys

import m5
from m5.objects import *

# Import MyCacheSystem from the parent directory's msi_caches.py
sys.path.insert(0, os.path.join(os.path.dirname(os.path.realpath(__file__)), ".."))
from msi_caches import MyCacheSystem

system = System()
system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "1GHz"
system.clk_domain.voltage_domain = VoltageDomain()
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MiB")]

system.cpu = [RiscvTimingSimpleCPU() for i in range(2)]

system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR3_1600_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]

for cpu in system.cpu:
    cpu.createInterruptController()

system.caches = MyCacheSystem()
system.caches.setup(system, system.cpu, [system.mem_ctrl])

# RISC-V TimingSimpleCPU does not need eviction notifications
# (no x86 mwait, no ARM exclusive monitor)
for ctrl in system.caches.controllers:
    if hasattr(ctrl, "send_evictions"):
        ctrl.send_evictions = False

thispath = os.path.dirname(os.path.realpath(__file__))
binary = os.path.join(
    thispath,
    "../../../../",
    "tests/test-progs/threads/bin/riscv/linux/threads",
)

process = Process()
process.cmd = [binary]
for cpu in system.cpu:
    cpu.workload = process
    cpu.createThreads()

system.workload = SEWorkload.init_compatible(binary)

root = Root(full_system=False, system=system)
m5.instantiate()
print("Beginning simulation!")
exit_event = m5.simulate()
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
