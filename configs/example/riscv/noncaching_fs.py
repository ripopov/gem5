# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause

"""
RISC-V full-system configuration for the functional CPU models.

One hart of NonCachingSimpleCPU (or AtomicSimpleCPU) on the HiFive platform
with an atomic, cache-less memory system: the fastest way gem5 executes
RISC-V code, and the configuration the util/riscv-bench harness measures.
The ISA reports the RVA23S64 profile, misaligned access to main memory is
supported and the hypervisor extension is enabled, all advertised in the
generated device tree.

Bare-metal ELF (M-mode at the ELF entry, exit via m5_exit):

    gem5.fast configs/example/riscv/noncaching_fs.py baremetal coremark.elf

Linux through OpenSBI fw_jump, with a kernel and an initramfs:

    gem5.fast configs/example/riscv/noncaching_fs.py linux \\
        --bootloader fw_jump.elf --kernel vmlinux --initrd rootfs.cpio
"""

import argparse
import os

import m5
from m5.objects import (
    AddrRange,
    BadAddr,
    Bridge,
    HiFive,
    IOXBar,
    PMAChecker,
    RiscvAtomicSimpleCPU,
    RiscvBareMetal,
    RiscvBootloaderKernelWorkload,
    RiscvNonCachingSimpleCPU,
    RiscvRTC,
    RiscvSystem,
    RiscvTimingSimpleCPU,
    Root,
    SimpleMemory,
    SrcClockDomain,
    SystemXBar,
    VoltageDomain,
)
from m5.util.convert import toFrequency
from m5.util.fdthelper import (
    Fdt,
    FdtNode,
    FdtPropertyStrings,
    FdtPropertyWords,
    FdtState,
)

parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[1])
sub = parser.add_subparsers(dest="mode", required=True)
baremetal = sub.add_parser("baremetal", help="run a bare-metal ELF")
baremetal.add_argument("binary")
linux = sub.add_parser("linux", help="boot Linux via an SBI bootloader")
linux.add_argument("--bootloader", required=True, help="OpenSBI fw_jump ELF")
linux.add_argument("--kernel", required=True, help="vmlinux")
linux.add_argument("--initrd", required=True, help="initramfs cpio")
linux.add_argument("--command-line", default="console=ttyS0")
for p in (baremetal, linux):
    p.add_argument(
        "--cpu-type",
        choices=("noncaching", "atomic", "timing"),
        default="noncaching",
        help="NonCachingSimpleCPU (default), AtomicSimpleCPU or "
        "TimingSimpleCPU; the memory mode follows the CPU",
    )
    p.add_argument("--mem-size", default="1GiB")
    p.add_argument("--cpu-clock", default="1GHz")
    p.add_argument(
        "--rtc-frequency",
        default="10MHz",
        help="rate of the CLINT mtime counter, advertised as the timebase",
    )
    p.add_argument("--vlen", type=int, default=256)
    p.add_argument("--max-ticks", type=int, default=m5.MaxTick)
    p.add_argument(
        "--stats-period",
        type=int,
        default=0,
        help="dump statistics every this many ticks as well as at the end",
    )
args = parser.parse_args()

rtc_frequency = int(toFrequency(args.rtc_frequency))

# --- Platform ---------------------------------------------------------------

system = RiscvSystem()
system.mem_mode = {
    "noncaching": "atomic_noncaching",
    "atomic": "atomic",
    "timing": "timing",
}[args.cpu_type]
system.mem_ranges = [AddrRange(start=0x80000000, size=args.mem_size)]
system.cache_line_size = 64
system.voltage_domain = VoltageDomain(voltage="1V")
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=system.voltage_domain
)
system.cpu_clk_domain = SrcClockDomain(
    clock=args.cpu_clock, voltage_domain=system.voltage_domain
)

system.membus = SystemXBar()
system.system_port = system.membus.cpu_side_ports
system.iobus = IOXBar()
system.iobus.badaddr_responder = BadAddr()
system.iobus.default = system.iobus.badaddr_responder.pio
system.bridge = Bridge(delay="50ns")
system.bridge.mem_side_port = system.iobus.cpu_side_ports
system.bridge.cpu_side_port = system.membus.mem_side_ports

system.platform = HiFive()
system.platform.rtc = RiscvRTC(frequency=args.rtc_frequency)
system.platform.clint.int_pin = system.platform.rtc.int_pin
system.platform.pci_host.internal_connect()
system.platform.pci_host.connect_upper_bus(system.iobus, True)
system.platform.attachOnChipIO(system.membus)
system.platform.attachOffChipIO(system.iobus)
system.platform.attachPlic()
system.platform.setNumCores(1)
system.bridge.ranges = system.platform._off_chip_ranges()

system.mem_ctrl = SimpleMemory(range=system.mem_ranges[0], latency="0ns")
system.mem_ctrl.port = system.membus.mem_side_ports

# --- CPU --------------------------------------------------------------------

cpu_class = {
    "noncaching": RiscvNonCachingSimpleCPU,
    "atomic": RiscvAtomicSimpleCPU,
    "timing": RiscvTimingSimpleCPU,
}[args.cpu_type]
system.cpu = cpu_class(clk_domain=system.cpu_clk_domain, cpu_id=0)
system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports
system.cpu.mmu.connectWalkerPorts(
    system.membus.cpu_side_ports, system.membus.cpu_side_ports
)
system.cpu.createInterruptController()
system.cpu.createThreads()

isa = system.cpu.isa[0]
isa.riscv_profile = "RVA23S64"
isa.vlen = args.vlen
isa.privilege_mode_set = "MHSU"
system.cpu.mmu.pma_checker = PMAChecker(
    uncacheable=[
        *system.platform._on_chip_ranges(),
        *system.platform._off_chip_ranges(),
    ],
    # Zicclsm: main memory supports misaligned loads and stores.
    misaligned=system.mem_ranges,
)

# --- Workload ---------------------------------------------------------------


def generate_dtb(path):
    state = FdtState(addr_cells=2, size_cells=2, cpu_cells=1)
    root = FdtNode("/")
    root.append(state.addrCellsProperty())
    root.append(state.sizeCellsProperty())
    root.appendCompatible(["riscv-virtio"])
    for mem_range in system.mem_ranges:
        node = FdtNode(f"memory@{int(mem_range.start):x}")
        node.append(FdtPropertyStrings("device_type", ["memory"]))
        node.append(
            FdtPropertyWords(
                "reg",
                state.addrCells(mem_range.start)
                + state.sizeCells(mem_range.size()),
            )
        )
        root.append(node)
    for section in (system.cpu, system.platform):
        for node in section.generateDeviceTree(state):
            if node.get_name() == root.get_name():
                root.merge(node)
            else:
                root.append(node)
    fdt = Fdt()
    fdt.add_rootnode(root)
    fdt.writeDtbFile(path)


if args.mode == "baremetal":
    system.workload = RiscvBareMetal(bootloader=args.binary)
else:
    dtb_path = os.path.join(m5.options.outdir, "device.dtb")
    generate_dtb(dtb_path)
    system.workload = RiscvBootloaderKernelWorkload(
        bootloader_filename=args.bootloader,
        bootloader_addr=0x80000000,
        object_file=args.kernel,
        kernel_addr=0x80200000,
        entry_point=0x80000000,
        dtb_filename=dtb_path,
        dtb_addr=0x87E00000,
        initrd_filename=args.initrd,
        initrd_addr=0x88000000,
        command_line=args.command_line,
        exit_on_kernel_panic=True,
    )

root = Root(full_system=True, system=system)
m5.instantiate()

print(f"ISA: {isa.get_isa_string()}")
if args.stats_period:
    m5.stats.periodicStatDump(args.stats_period)
exit_event = m5.simulate(args.max_ticks)
cause = exit_event.getCause()
print(f"Exiting @ tick {m5.curTick()} because {cause}")
if exit_event.getCode() != 0:
    print(f"Guest exit code: {exit_event.getCode()}")
if cause.startswith("m5_exit") or cause.startswith("exiting with"):
    exit(exit_event.getCode())
exit(1)
