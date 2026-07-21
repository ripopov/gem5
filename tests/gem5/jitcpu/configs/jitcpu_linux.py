import argparse
import os

import m5
from m5.defines import buildEnv
from m5.objects import (
    AddrRange,
    Bridge,
    DDR3_1600_8x8,
    HiFive,
    IOXBar,
    MemCtrl,
    PMAChecker,
    RiscvJitCPU,
    RiscvLinux,
    RiscvRTC,
    RiscvSystem,
    Root,
    SrcClockDomain,
    SystemXBar,
    VoltageDomain,
)
from m5.util import addToPath
from m5.util.fdthelper import (
    Fdt,
    FdtNode,
    FdtPropertyStrings,
    FdtPropertyWords,
    FdtState,
)
from m5.objects.RiscvCPU import RiscvNonCachingSimpleCPU, RiscvO3CPU
from m5.stats.gem5stats import get_simstat

addToPath(os.path.join(m5.util.repoPath(), "configs"))

from common import Options
from ruby import Ruby


def generate_dtb(system, output):
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

    for section in [system.cpu, system.platform]:
        for node in section.generateDeviceTree(state):
            if node.get_name() == root.get_name():
                root.merge(node)
            else:
                root.append(node)

    fdt = Fdt()
    fdt.add_rootnode(root)
    fdt.writeDtbFile(output)


def ruby_options():
    parser = argparse.ArgumentParser(add_help=False)
    Options.addCommonOptions(parser)
    Ruby.define_options(parser)
    options = parser.parse_args([])
    options.num_cpus = 1
    options.num_dirs = 1
    options.num_l3caches = 1
    options.topology = "Crossbar"
    return options


def ruby_router_messages(ruby_system):
    network_stats = get_simstat(ruby_system.network)
    routers = network_stats["routers"].values["value"]
    return int(
        sum(
            group["total_msg_count"].value
            for router in routers
            for name, group in router.values.items()
            if name.startswith("throttle")
        )
    )


parser = argparse.ArgumentParser()
parser.add_argument("linux_image")
parser.add_argument("backend")
parser.add_argument("--cpu", choices=("jit", "noncaching"), default="jit")
parser.add_argument("--rtc-frequency", default="1MHz")
parser.add_argument("--max-ticks", type=int, default=2_000_000_000_000)
parser.add_argument("--switch-to-o3", action="store_true")
parser.add_argument("--o3-ticks", type=int, default=10_000_000)
parser.add_argument("--ruby-chi", action="store_true")
args = parser.parse_args()

if args.ruby_chi and buildEnv["PROTOCOL"] != "CHI":
    parser.error("--ruby-chi requires a gem5 binary built with PROTOCOL=CHI")

system = RiscvSystem()
system.mem_mode = "atomic_noncaching"
system.mem_ranges = [AddrRange(start=0x80000000, size="256MiB")]

system.iobus = IOXBar()
if not args.ruby_chi:
    system.membus = SystemXBar()
    system.system_port = system.membus.cpu_side_ports

system.platform = HiFive()
system.platform.rtc = RiscvRTC(frequency=args.rtc_frequency)
system.platform.clint.int_pin = system.platform.rtc.int_pin
system.platform.pci_host.internal_connect()
system.platform.pci_host.connect_upper_bus(system.iobus, True)
if args.ruby_chi:
    system.platform.attachOnChipIO(system.iobus)
    system.platform.attachOffChipIO(system.iobus)
else:
    system.platform.attachOnChipIO(system.membus)
    system.platform.attachOffChipIO(system.iobus)
system.platform.attachPlic()
system.platform.setNumCores(1)

if not args.ruby_chi:
    system.bridge = Bridge(delay="50ns")
    system.bridge.mem_side_port = system.iobus.cpu_side_ports
    system.bridge.cpu_side_port = system.membus.mem_side_ports
    system.bridge.ranges = system.platform._off_chip_ranges()

system.cache_line_size = 64
system.voltage_domain = VoltageDomain(voltage="1V")
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=system.voltage_domain
)
system.cpu_voltage_domain = VoltageDomain()
system.cpu_clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=system.cpu_voltage_domain
)

if args.cpu == "jit":
    system.cpu = RiscvJitCPU(
        clk_domain=system.cpu_clk_domain,
        cpu_id=0,
        backend_path=args.backend,
        batch_size=1024,
    )
else:
    system.cpu = RiscvNonCachingSimpleCPU(
        clk_domain=system.cpu_clk_domain,
        cpu_id=0,
    )

cpus = [system.cpu]
if args.switch_to_o3:
    if args.cpu != "jit":
        parser.error("--switch-to-o3 requires --cpu jit")
    system.o3 = RiscvO3CPU(
        clk_domain=system.cpu_clk_domain,
        cpu_id=0,
        switched_out=True,
    )
    cpus.append(system.o3)

for cpu in cpus:
    if cpu is system.cpu and not args.ruby_chi:
        cpu.icache_port = system.membus.cpu_side_ports
        cpu.dcache_port = system.membus.cpu_side_ports
        cpu.mmu.connectWalkerPorts(
            system.membus.cpu_side_ports, system.membus.cpu_side_ports
        )
    cpu.createInterruptController()
    cpu.createThreads()
    cpu.isa[0].enable_rvv = False
    cpu.isa[0].enable_Zicbom_fs = False
    cpu.isa[0].enable_Zicboz_fs = False
    cpu.mmu.pma_checker = PMAChecker(
        uncacheable=[
            *system.platform._on_chip_ranges(),
            *system.platform._off_chip_ranges(),
        ]
    )

if args.ruby_chi:
    ruby_args = ruby_options()
    Ruby.create_system(
        ruby_args,
        True,
        system,
        system.iobus,
        dma_ports=[],
        bootmem=None,
        cpus=[system.cpu],
    )
    system.ruby.clk_domain = SrcClockDomain(
        clock=ruby_args.ruby_clock,
        voltage_domain=system.voltage_domain,
    )
    system.iobus.mem_side_ports = system.ruby._io_port.in_ports
    system.ruby._cpu_ports[0].connectCpuPorts(system.cpu)
else:
    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR3_1600_8x8(range=system.mem_ranges[0])
    system.mem_ctrl.port = system.membus.mem_side_ports

dtb_path = os.path.join(m5.options.outdir, "device.dtb")
generate_dtb(system, dtb_path)
system.workload = RiscvLinux(
    object_file=args.linux_image,
    dtb_filename=dtb_path,
    dtb_addr=0x87E00000,
    command_line="console=ttyS0",
    addr_check=False,
)

root = Root(full_system=True, system=system)
m5.instantiate()
exit_event = m5.simulate(args.max_ticks)
print(
    f"JitCPU Linux stopped @ tick {m5.curTick()}: "
    f"{exit_event.getCause()}"
)

if args.switch_to_o3:
    if exit_event.getCause() != "m5_exit instruction encountered":
        raise RuntimeError("Linux did not reach its userspace m5 exit")

    if args.ruby_chi:
        jit_ruby_messages = ruby_router_messages(system.ruby)
        print(
            "JitCPU generated "
            f"{jit_ruby_messages} timing Ruby CHI messages"
        )
        if jit_ruby_messages != 0:
            raise RuntimeError(
                "JitCPU unexpectedly populated CHI during uncached boot"
            )

    m5.switchCpus(
        system,
        [(system.cpu, system.o3)],
        is_ruby=args.ruby_chi,
    )
    print(
        f"Switched JitCPU -> O3CPU @ tick {m5.curTick()}, "
        f"memory mode {system.getMemoryMode()}"
    )
    m5.stats.reset()
    exit_event = m5.simulate(args.o3_ticks)
    print(
        f"O3CPU continued @ tick {m5.curTick()}: "
        f"{exit_event.getCause()}"
    )
    if exit_event.getCause() != "simulate() limit reached":
        raise RuntimeError("O3CPU did not complete its validation interval")

    o3_user_insts = int(
        Root.getInstance()
        .resolveStat("system.o3.commitStats0.numUserInsts")
        .value
    )
    print(f"O3CPU committed {o3_user_insts} userspace instructions")
    if o3_user_insts == 0:
        raise RuntimeError("O3CPU made no userspace progress after takeover")

    if args.ruby_chi:
        ruby_messages = ruby_router_messages(system.ruby)
        print(
            "Ruby CHI carried "
            f"{ruby_messages} router-link messages after takeover"
        )
        if ruby_messages == 0:
            raise RuntimeError("Ruby CHI carried no post-takeover traffic")
