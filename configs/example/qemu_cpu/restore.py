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

The board mirrors the QEMU 'virt' machine closely enough for the
snapshotted Linux kernel to keep running: the HiFive platform's CLINT
(0x02000000), PLIC (0x0c000000) and 16550 UART (0x10000000) sit at the
same addresses, DRAM is at 0x80000000, and the CLINT RTC ticks at the
10 MHz timebase QEMU advertised to the guest.

Usage:
    build/RISCV/gem5.opt configs/example/qemu_cpu/restore.py \\
        --snapshot-dir snapshots/snap --cpu o3 --max-insts 20000000
"""
import argparse
import json
import os
import struct

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
parser.add_argument("--timer-gap", type=int, default=200000,
                    help="clamp the restored CLINT timer to fire at most this "
                         "many mtime ticks after mtime, so gem5 need not "
                         "fast-forward through millions of idle RTC ticks. "
                         "An early timer interrupt is harmless to Linux. "
                         "0 disables clamping (exact restore).")
args = parser.parse_args()

# --------------------------------------------------------------------------
# Read the snapshot metadata
# --------------------------------------------------------------------------
snap = os.path.abspath(args.snapshot_dir)
with open(os.path.join(snap, "meta.json")) as f:
    meta = json.load(f)

ram_file = os.path.join(snap, meta["ram_file"])
regs_file = os.path.join(snap, meta["regs_file"])
ram_base = meta["guest_ram_base"]
ram_size = meta["guest_ram_size"]
clint_base = meta["clint_base"]

# CLINT timer state lives in the MMIO dump: mtimecmp[0] @ 0x4000, mtime @ 0xBFF8
with open(os.path.join(snap, meta["clint_file"]), "rb") as f:
    clint_bytes = f.read()
mtimecmp = struct.unpack_from("<Q", clint_bytes, 0x4000)[0]
mtime = struct.unpack_from("<Q", clint_bytes, 0xBFF8)[0]

# Avoid a multi-million-event idle fast-forward: let the first timer
# interrupt fire soon after restore.  Linux treats an early tick as benign.
if args.timer_gap > 0 and mtimecmp > mtime + args.timer_gap:
    print("[restore] clamping mtimecmp %#x -> %#x (timer-gap=%d)"
          % (mtimecmp, mtime + args.timer_gap, args.timer_gap))
    mtimecmp = mtime + args.timer_gap

print("[restore] snapshot   : %s" % snap)
print("[restore] guest RAM  : %#x + %d MiB" % (ram_base, ram_size >> 20))
print("[restore] entry pc   : %#x" % meta.get("pc", 0))
print("[restore] CLINT      : mtime=%#x mtimecmp=%#x" % (mtime, mtimecmp))
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
system.mem_ranges = [AddrRange(start=ram_base, size=ram_size)]
system.cache_line_size = 64

system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(
    clock=args.clock, voltage_domain=system.voltage_domain
)

system.membus = SystemXBar()
system.iobus = IOXBar()
system.system_port = system.membus.cpu_side_ports

# Main memory.  A SimpleMemory keeps the model small; its contents are
# overwritten by the snapshot's RAM image in the workload's initState().
system.mem_ctrl = SimpleMemory(range=system.mem_ranges[0], latency="30ns")
system.mem_ctrl.port = system.membus.mem_side_ports

# --------------------------------------------------------------------------
# HiFive platform -- addresses chosen to match the QEMU 'virt' machine
# --------------------------------------------------------------------------
system.platform = HiFive()
system.platform.rtc = RiscvRTC(frequency="10MHz")  # QEMU virt timebase
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

# membus -> iobus for CPU accesses to off-chip devices (the UART)
system.bridge = Bridge(delay="50ns")
system.bridge.mem_side_port = system.iobus.cpu_side_ports
system.bridge.cpu_side_port = system.membus.mem_side_ports
system.bridge.ranges = system.platform._off_chip_ranges()

# iobus -> membus so device DMA can reach memory
system.iobridge = Bridge(delay="50ns", ranges=system.mem_ranges)
system.iobridge.cpu_side_port = system.iobus.mem_side_ports
system.iobridge.mem_side_port = system.membus.cpu_side_ports

system.platform.attachOnChipIO(system.membus)
system.platform.attachOffChipIO(system.iobus)
system.platform.attachPlic()
system.platform.setNumCores(1)

# --------------------------------------------------------------------------
# CPU
# --------------------------------------------------------------------------
system.cpu = CPUClass(clk_domain=system.clk_domain, cpu_id=0)
system.cpu.createThreads()
system.cpu.createInterruptController()
system.cpu.connectBus(system.membus)

uncacheable = [
    *system.platform._on_chip_ranges(),
    *system.platform._off_chip_ranges(),
]
system.cpu.mmu.pma_checker = PMAChecker(uncacheable=uncacheable)

if args.max_insts > 0:
    system.cpu.max_insts_any_thread = args.max_insts

# --------------------------------------------------------------------------
# Workload: restore the QEMU snapshot instead of booting a kernel
# --------------------------------------------------------------------------
system.workload = RiscvQemuSnapshotWorkload(
    ram_file=ram_file,
    ram_addr=ram_base,
    regs_file=regs_file,
    clint_addr=clint_base,
    clint_mtime=mtime,
    clint_mtimecmp=mtimecmp,
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
