import argparse

import m5
from m5.objects import (
    AddrRange,
    Bridge,
    DDR3_1600_8x8,
    HiFive,
    IOXBar,
    MemCtrl,
    PMAChecker,
    RiscvBareMetal,
    RiscvJitCPU,
    RiscvRTC,
    RiscvSystem,
    Root,
    SrcClockDomain,
    SystemXBar,
    VoltageDomain,
)
from m5.util.convert import toMemorySize
from m5.objects.RiscvCPU import RiscvO3CPU


parser = argparse.ArgumentParser()
parser.add_argument("binary")
parser.add_argument("backend")
parser.add_argument("--max-ticks", type=int, default=100000)
parser.add_argument("--switch-to-o3", action="store_true")
args = parser.parse_args()

memory_start = 0x7FFFF000
reset_vector_size = 0x1000

system = RiscvSystem()
system.mem_mode = "atomic_noncaching"
system.mem_ranges = [
    AddrRange(
        start=memory_start,
        size=toMemorySize("128MiB") + reset_vector_size,
    )
]
system.workload = RiscvBareMetal(bootloader=args.binary)

system.iobus = IOXBar()
system.membus = SystemXBar()
system.system_port = system.membus.cpu_side_ports

system.platform = HiFive()
system.platform.rtc = RiscvRTC(frequency="100MHz")
system.platform.clint.int_pin = system.platform.rtc.int_pin
system.platform.pci_host.internal_connect()
system.platform.pci_host.connect_upper_bus(system.iobus, True)
system.platform.attachOnChipIO(system.membus)
system.platform.attachOffChipIO(system.iobus)
system.platform.attachPlic()
system.platform.setNumCores(1)

system.bridge = Bridge(delay="50ns")
system.bridge.mem_side_port = system.iobus.cpu_side_ports
system.bridge.cpu_side_port = system.membus.mem_side_ports
system.bridge.ranges = system.platform._off_chip_ranges()

system.iobridge = Bridge(delay="50ns", ranges=system.mem_ranges)
system.iobridge.cpu_side_port = system.iobus.mem_side_ports
system.iobridge.mem_side_port = system.membus.cpu_side_ports

system.cache_line_size = 64
system.voltage_domain = VoltageDomain(voltage="1V")
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=system.voltage_domain
)
system.cpu_voltage_domain = VoltageDomain()
system.cpu_clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=system.cpu_voltage_domain
)

system.cpu = RiscvJitCPU(
    clk_domain=system.cpu_clk_domain,
    cpu_id=0,
    backend_path=args.backend,
    batch_size=256,
)
system.cpu.icache_port = system.membus.cpu_side_ports
system.cpu.dcache_port = system.membus.cpu_side_ports
system.cpu.mmu.connectWalkerPorts(
    system.membus.cpu_side_ports, system.membus.cpu_side_ports
)
system.cpu.createInterruptController()
system.cpu.createThreads()
system.cpu.isa[0].enable_rvv = False
system.cpu.isa[0].enable_Zicbom_fs = False
system.cpu.isa[0].enable_Zicboz_fs = False

uncacheable_range = [
    *system.platform._on_chip_ranges(),
    *system.platform._off_chip_ranges(),
]
system.cpu.mmu.pma_checker = PMAChecker(uncacheable=uncacheable_range)

if args.switch_to_o3:
    system.o3 = RiscvO3CPU(
        clk_domain=system.cpu_clk_domain,
        cpu_id=0,
        switched_out=True,
    )
    system.o3.createInterruptController()
    system.o3.createThreads()
    system.o3.isa[0].enable_rvv = False
    system.o3.isa[0].enable_Zicbom_fs = False
    system.o3.isa[0].enable_Zicboz_fs = False
    system.o3.mmu.pma_checker = PMAChecker(
        uncacheable=uncacheable_range
    )

system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR3_1600_8x8(range=system.mem_ranges[0])
system.mem_ctrl.port = system.membus.mem_side_ports

root = Root(full_system=True, system=system)
m5.instantiate()

exit_event = m5.simulate(args.max_ticks)
print(
    f"JitCPU smoke stopped @ tick {m5.curTick()}: "
    f"{exit_event.getCause()}"
)

if args.switch_to_o3:
    if exit_event.getCause() != "switchcpu":
        raise RuntimeError("JitCPU did not execute m5_switch_cpu")
    m5.switchCpus(system, [(system.cpu, system.o3)])
    print(
        f"Switched JitCPU -> O3CPU @ tick {m5.curTick()}, "
        f"memory mode {system.getMemoryMode()}"
    )
    m5.stats.reset()
    exit_event = m5.simulate(args.max_ticks)
    print(
        f"O3CPU switch smoke stopped @ tick {m5.curTick()}: "
        f"{exit_event.getCause()}"
    )
