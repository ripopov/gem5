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

"""gem5 QEMU-snapshot mode -- stage 3 of 3.

Restore a machine snapshot captured under QEMU (util/qemu-snap/scripts/
qemu-snapshot.py) into a RISC-V full-system simulation and continue
execution on one of gem5's detailed CPU models.

The HiFive board is configured from the snapshot's meta.json, whose platform
section is derived from the QEMU 'virt' device tree -- CLINT/PLIC/UART
addresses, DRAM base/size, hart count and the CLINT timebase all come from
there rather than being hand-matched.

Two memory subsystems are supported:

  * classic  (default) -- a SystemXBar with a single SimpleMemory; fast, no
    coherence modelling.
  * Ruby     (--ruby)  -- the Ruby coherent cache subsystem running the CHI
    protocol (Arm AMBA CHI): per-core private L1+L2 caches, distributed L3
    home nodes, and a real DRAM controller, wired up by gem5's standard CHI
    configuration scripts (configs/ruby/CHI.py + configs/ruby/CHI_config.py).
    The interconnect is a CustomMesh NoC described by a standard CHI NoC
    config script (default: configs/example/noc_config/2x4.py).

Usage:
    build/RISCV/gem5.opt configs/example/qemu_snap/restore.py \\
        --snapshot-dir snapshots/bench --cpu o3

    build/RISCV/gem5.opt configs/example/qemu_snap/restore.py \\
        --snapshot-dir snapshots/philo --cpu o3 --ruby
"""
import argparse
import json
import os
import sys

import m5
from m5.objects import *
from m5.util import addToPath

# Ruby / network / topology helpers live under configs/.
addToPath("../..")

# --------------------------------------------------------------------------
# Ruby is opt-in.  Scan argv early: the Ruby option machinery (and the
# multi-protocol gem5 binary) is only pulled in when --ruby is requested, so
# a plain classic-memory restore needs none of it and stays argument-clean.
# --------------------------------------------------------------------------
USE_RUBY = "--ruby" in sys.argv

# --------------------------------------------------------------------------
# Arguments
# --------------------------------------------------------------------------
parser = argparse.ArgumentParser(description=__doc__,
    formatter_class=argparse.RawDescriptionHelpFormatter)
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
parser.add_argument("--ruby", action="store_true",
                    help="use the Ruby coherent memory subsystem (CHI "
                         "protocol) instead of the classic SystemXBar + "
                         "SimpleMemory")

# Path to the standard CHI NoC config script that describes the CustomMesh.
NOC_CONFIG = os.path.join(sys.path[0], "..", "noc_config", "2x4.py")

if USE_RUBY:
    # The RISCV gem5 binary is built with several Ruby protocols; the Ruby
    # option machinery insists on an explicit --protocol.  qemu-snap supports
    # exactly one -- CHI -- so it is pinned here and not exposed to callers.
    if not any(a == "--protocol" or a.startswith("--protocol=")
               for a in sys.argv):
        sys.argv += ["--protocol", "CHI"]

    from ruby import Ruby

    # Cache / home-node / DRAM knobs consumed by the CHI configuration
    # (configs/ruby/CHI.py + CHI_config.py) and by Ruby's memory-controller
    # setup.  These normally come from the common Options.py; qemu-snap only
    # needs this handful, so they are spelled out.
    parser.add_argument("--num-dirs", type=int, default=1,
                        help="number of CHI memory (SNF) / DRAM controllers")
    parser.add_argument("--num-l3caches", type=int, default=4,
                        help="number of CHI L3 home nodes (HNF)")
    parser.add_argument("--cacheline-size", type=int, default=64,
                        help="cache line size in bytes")
    parser.add_argument("--l1i-size", type=str, default="32KiB")
    parser.add_argument("--l1i-assoc", type=int, default=4)
    parser.add_argument("--l1d-size", type=str, default="32KiB")
    parser.add_argument("--l1d-assoc", type=int, default=4)
    parser.add_argument("--l2-size", type=str, default="256KiB")
    parser.add_argument("--l2-assoc", type=int, default=8)
    parser.add_argument("--l3-size", type=str, default="1MiB")
    parser.add_argument("--l3-assoc", type=int, default=16)
    parser.add_argument("--mem-type", type=str, default="DDR3_1600_8x8",
                        help="DRAM model for the CHI memory controller")
    parser.add_argument("--enable-dram-powerdown", action="store_true",
                        help="enable low-power DRAM states")
    # Adds --ruby-clock, --topology, --network, --protocol, --chi-config, ...
    Ruby.define_options(parser)
    # CHI is built around a NoC; default to the CustomMesh described by the
    # standard CHI NoC config script rather than the generic Crossbar.
    parser.set_defaults(topology="CustomMesh")

args = parser.parse_args()

if USE_RUBY and args.topology == "CustomMesh" and not args.chi_config:
    # CustomMesh needs a NoC config script; use the standard 2x4 example.
    args.chi_config = os.path.abspath(NOC_CONFIG)

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

print("[restore] snapshot   : %s (test=%s)"
      % (snap, meta.get("test", meta.get("mode", "?"))))
print("[restore] platform   : ram=%#x+%dMiB clint=%#x plic=%#x uart=%#x"
      % (ram["base"], ram["size"] >> 20, clint["base"], plic["base"],
         uart["base"]))
print("[restore] harts      : %d   timebase=%d Hz"
      % (num_harts, meta["timebase"]))
print("[restore] CPU model  : %s" % args.cpu)
if args.ruby:
    print("[restore] memory     : Ruby CHI (%s network, %s topology)"
          % (args.network, args.topology))
else:
    print("[restore] memory     : classic (SystemXBar + SimpleMemory)")

CPU_CLASSES = {
    "atomic": RiscvAtomicSimpleCPU,
    "timing": RiscvTimingSimpleCPU,
    "o3": RiscvO3CPU,
    "minor": RiscvMinorCPU,
}
CPUClass = CPU_CLASSES[args.cpu]

if args.ruby and args.cpu == "atomic":
    m5.util.fatal("--ruby requires a timing CPU model (timing/o3/minor); "
                  "AtomicSimpleCPU has no timing memory protocol")

# Ruby is a timing-only memory system.
mem_mode = "atomic" if (args.cpu == "atomic" and not args.ruby) else "timing"

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

# --------------------------------------------------------------------------
# Memory interconnect
#
# classic: CPUs hang off a SystemXBar that also carries on-chip IO; off-chip
#          IO sits behind a bridge on a separate IOXBar.
# Ruby:    CPUs reach memory through Ruby; all platform PIO devices sit on a
#          single piobus that the Ruby sequencers drive directly.
# --------------------------------------------------------------------------
if not args.ruby:
    system.membus = SystemXBar()
    system.iobus = IOXBar()
    system.system_port = system.membus.cpu_side_ports

    # Catch-all for unmapped physical addresses.  A detailed CPU (notably
    # MinorCPU) speculatively issues load requests down mispredicted paths;
    # with no caches such a stray access reaches the crossbar directly, and a
    # bare SystemXBar fatals on any address no port claims.  A BadAddr
    # responder turns those into ordinary bad-address responses -- the CPU
    # squashes the wrong-path instruction, so the response is harmless, and a
    # genuine unmapped access still becomes a proper access fault.  This
    # mirrors gem5's standard NoCache hierarchy (no_cache.py).
    system.membus.badaddr_responder = BadAddr()
    system.membus.default = system.membus.badaddr_responder.pio

    system.mem_ctrl = SimpleMemory(range=system.mem_ranges[0], latency="30ns")
    system.mem_ctrl.port = system.membus.mem_side_ports
else:
    # All platform PIO devices share one bus; the Ruby sequencers' PIO ports
    # are wired to it by Ruby.create_system(..., piobus=...).
    system.piobus = IOXBar()
    # Catch-all for unmapped addresses.  A detailed CPU speculatively issues
    # wrong-path loads; under Ruby a stray non-memory address is routed by
    # the sequencer out to the piobus, and a bare IOXBar fatals on any
    # address no device claims.  A BadAddr responder turns those into
    # ordinary bad-address responses -- the CPU squashes the wrong-path
    # instruction, so the response is harmless.
    system.piobus.badaddr_responder = BadAddr()
    system.piobus.default = system.piobus.badaddr_responder.pio
    # A small private IOXBar carries only the (unused) PCI host's self-wiring
    # so none of its ports dangle.
    system.iobus = IOXBar()

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

if not args.ruby:
    # Bridge the on-chip membus to the off-chip iobus both ways.
    system.bridge = Bridge(delay="50ns")
    system.bridge.mem_side_port = system.iobus.cpu_side_ports
    system.bridge.cpu_side_port = system.membus.mem_side_ports
    system.bridge.ranges = system.platform._off_chip_ranges()

    system.iobridge = Bridge(delay="50ns", ranges=system.mem_ranges)
    system.iobridge.cpu_side_port = system.iobus.mem_side_ports
    system.iobridge.mem_side_port = system.membus.cpu_side_ports

    system.platform.attachOnChipIO(system.membus)
    system.platform.attachOffChipIO(system.iobus)
else:
    # With Ruby every platform device lives on the single piobus.
    system.platform.attachOnChipIO(system.piobus)
    system.platform.attachOffChipIO(system.piobus)

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
    cpu.mmu.pma_checker = PMAChecker(uncacheable=uncacheable)
    if args.max_insts > 0:
        cpu.max_insts_any_thread = args.max_insts
    if not args.ruby:
        cpu.connectBus(system.membus)

# --------------------------------------------------------------------------
# Ruby memory subsystem (built after the CPUs so their ports can be wired)
# --------------------------------------------------------------------------
if args.ruby:
    # CHI's create_system asserts num_cpus == len(cpus); for a snapshot
    # restore that is exactly the hart count.
    args.num_cpus = num_harts
    system.cache_line_size = args.cacheline_size

    # Build the CHI cache hierarchy + NoC.  Ruby.create_system dispatches to
    # configs/ruby/CHI.py, which creates one request node (RNF, private
    # L1+L2) per hart, the L3 home nodes (HNF), the CHI memory nodes (SNF)
    # and the misc node, then lays them out on the CustomMesh; Ruby then
    # attaches a DRAM controller to each SNF.
    Ruby.create_system(
        args,
        True,             # full_system
        system,
        system.piobus,    # platform PIO devices reachable via the sequencers
        [],               # no DMA devices on this HiFive board
        None,             # no separate boot ROM
        system.cpu,
    )
    system.ruby.clk_domain = SrcClockDomain(
        clock=args.ruby_clock, voltage_domain=system.voltage_domain
    )
    # The CHI IO request node forwards device-side accesses into Ruby; give
    # its sequencer a home on the piobus (mirrors a full-system Ruby config).
    system.piobus.mem_side_ports = system.ruby._io_port.in_ports

    # Wire each hart's CPU ports to its CHI request node's sequencers.
    for i, cpu in enumerate(system.cpu):
        system.ruby._cpu_ports[i].connectCpuPorts(cpu)

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
