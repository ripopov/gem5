# Copyright (c) 2026 The gem5 Authors
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

"""gem5 QEMU-CPU mode -- stage 3 of 3.

Restore a machine snapshot captured under QEMU (util/qemu-cpu/scripts/
qemu-snapshot.py) into a RISC-V full-system simulation and continue
execution on one of gem5's detailed CPU models.

The HiFive board is configured from the snapshot's meta.json, whose platform
section is derived from the QEMU 'virt' device tree -- CLINT/PLIC/UART
addresses, DRAM base/size, hart count and the CLINT timebase all come from
there rather than being hand-matched.

Usage:
    build/RISCV/gem5.opt configs/example/qemu_cpu/restore.py \\
        --snapshot-dir snapshots/bench --cpu o3
"""
import argparse
import json
import os

import m5
from m5.objects import *

# --------------------------------------------------------------------------
# Arguments
# --------------------------------------------------------------------------
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--snapshot-dir", required=True,
                    help="snapshot directory produced by qemu-snapshot.py")
parser.add_argument("--cpu", default="timing",
                    choices=["atomic", "timing", "o3", "minor"],
                    help="detailed CPU model to restore into")
parser.add_argument("--clock", default="1GHz", help="CPU/system clock")
parser.add_argument("--max-insts", type=int, default=0,
                    help="stop after N committed instructions (0 = unlimited)")
parser.add_argument("--max-ticks", type=int, default=0,
                    help="stop after N simulated ticks (0 = unlimited)")
parser.add_argument("--timer-gap", type=int, default=0,
                    help="clamp each restored CLINT timer to fire at most "
                         "this many mtime ticks after mtime (0 = exact "
                         "restore). An early timer interrupt is harmless to "
                         "Linux and avoids a long idle fast-forward.")
args = parser.parse_args()

# --------------------------------------------------------------------------
# Read the snapshot metadata (DTB-derived platform description)
# --------------------------------------------------------------------------
snap = os.path.abspath(args.snapshot_dir)
with open(os.path.join(snap, "meta.json")) as f:
    meta = json.load(f)


def spath(name):
    return os.path.join(snap, name)


ram = meta["ram"]
clint = meta["clint"]
plic = meta["plic"]
uart = meta["uart"]
num_harts = meta["num_harts"]
regs_files = [spath(h["regs_file"]) for h in meta["harts"]]

print("[restore] snapshot   : %s (mode=%s)" % (snap, meta.get("mode", "?")))
print("[restore] platform   : ram=%#x+%dMiB clint=%#x plic=%#x uart=%#x"
      % (ram["base"], ram["size"] >> 20, clint["base"], plic["base"],
         uart["base"]))
print("[restore] harts      : %d   timebase=%d Hz"
      % (num_harts, meta["timebase"]))
print("[restore] CPU model  : %s" % args.cpu)

CPU_CLASSES = {
    "atomic": RiscvAtomicSimpleCPU,
    "timing": RiscvTimingSimpleCPU,
    "o3": RiscvO3CPU,
    "minor": RiscvMinorCPU,
}
CPUClass = CPU_CLASSES[args.cpu]
mem_mode = "atomic" if args.cpu == "atomic" else "timing"

# --------------------------------------------------------------------------
# System
# --------------------------------------------------------------------------
system = RiscvSystem()
system.mem_mode = mem_mode
system.mem_ranges = [AddrRange(start=ram["base"], size=ram["size"])]
system.cache_line_size = 64

system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(
    clock=args.clock, voltage_domain=system.voltage_domain
)

system.membus = SystemXBar()
system.iobus = IOXBar()
system.system_port = system.membus.cpu_side_ports

system.mem_ctrl = SimpleMemory(range=system.mem_ranges[0], latency="30ns")
system.mem_ctrl.port = system.membus.mem_side_ports

# --------------------------------------------------------------------------
# HiFive platform -- addresses taken from the snapshot's (DTB-derived) meta
# --------------------------------------------------------------------------
system.platform = HiFive()
system.platform.clint.pio_addr = clint["base"]
system.platform.plic.pio_addr = plic["base"]
system.platform.uart.pio_addr = uart["base"]

# The CLINT mtime advances at one tick per RTC pulse: match the guest's
# timebase so the restored timer keeps the kernel's notion of time.
system.platform.rtc = RiscvRTC(frequency="%dHz" % meta["timebase"])
system.platform.clint.int_pin = system.platform.rtc.int_pin

# HiFive carries a (here unused) PCI host; wire it so no port dangles.
system.iobus.cpu_side_ports = system.platform.pci_host.up_request_port()
system.iobus.mem_side_ports = system.platform.pci_host.up_response_port()
system.platform.pci_bus.cpu_side_ports = (
    system.platform.pci_host.down_request_port()
)
system.platform.pci_bus.default = system.platform.pci_host.down_response_port()
system.platform.pci_bus.config_error_port = (
    system.platform.pci_host.config_error.pio
)

system.bridge = Bridge(delay="50ns")
system.bridge.mem_side_port = system.iobus.cpu_side_ports
system.bridge.cpu_side_port = system.membus.mem_side_ports
system.bridge.ranges = system.platform._off_chip_ranges()

system.iobridge = Bridge(delay="50ns", ranges=system.mem_ranges)
system.iobridge.cpu_side_port = system.iobus.mem_side_ports
system.iobridge.mem_side_port = system.membus.cpu_side_ports

system.platform.attachOnChipIO(system.membus)
system.platform.attachOffChipIO(system.iobus)
system.platform.attachPlic()
system.platform.setNumCores(num_harts)

# --------------------------------------------------------------------------
# CPUs -- one per hart
# --------------------------------------------------------------------------
system.cpu = [
    CPUClass(clk_domain=system.clk_domain, cpu_id=i)
    for i in range(num_harts)
]
uncacheable = [
    *system.platform._on_chip_ranges(),
    *system.platform._off_chip_ranges(),
]
for cpu in system.cpu:
    cpu.createThreads()
    cpu.createInterruptController()
    cpu.connectBus(system.membus)
    cpu.mmu.pma_checker = PMAChecker(uncacheable=uncacheable)
    if args.max_insts > 0:
        cpu.max_insts_any_thread = args.max_insts

# --------------------------------------------------------------------------
# Workload: restore the QEMU snapshot instead of booting a kernel
# --------------------------------------------------------------------------
system.workload = RiscvQemuSnapshotWorkload(
    ram_file=spath(ram["file"]),
    ram_addr=ram["base"],
    regs_files=regs_files,
    clint_addr=clint["base"],
    clint_file=spath(clint["file"]),
    clint_timer_gap=args.timer_gap,
    plic_addr=plic["base"],
    plic_file=spath(plic["file"]),
    plic_num_src=int(system.platform.plic.n_src),
    plic_num_contexts=2 * num_harts,   # hart_config is "MS" per hart
    uart_addr=uart["base"],
    uart_file=spath(uart["file"]),
)

# --------------------------------------------------------------------------
# Instantiate and run
# --------------------------------------------------------------------------
root = Root(full_system=True, system=system)
m5.instantiate()

print("[restore] starting simulation on %s CPU ..." % args.cpu)
limit = args.max_ticks if args.max_ticks > 0 else m5.MaxTick
exit_event = m5.simulate(limit)
print("[restore] exit @ tick %d : %s"
      % (m5.curTick(), exit_event.getCause()))
