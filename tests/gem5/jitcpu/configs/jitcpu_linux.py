import argparse
import os

import m5
from m5 import params
from m5.defines import buildEnv
from m5.objects import (
    AddrRange,
    Bridge,
    DDR3_1600_8x8,
    HiFive,
    IOXBar,
    MemCtrl,
    PMAChecker,
    RiscvBootloaderKernelWorkload,
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


def ruby_options(network):
    parser = argparse.ArgumentParser(add_help=False)
    Options.addCommonOptions(parser)
    Ruby.define_options(parser)
    options = parser.parse_args([])
    options.num_cpus = 1
    options.num_dirs = 1
    options.num_l3caches = 1
    options.topology = "Crossbar"
    options.network = network
    return options


def ruby_router_messages(ruby_system):
    if args.ruby_network == "garnet":
        return int(ruby_system.network.getTotalPacketsInjected())

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


def stat_value(name):
    value = Root.getInstance().resolveStat(name).value
    if isinstance(value, list):
        return int(sum(value))
    return int(value)


def cpu_user_instructions(cpu_name):
    return stat_value(f"system.{cpu_name}.commitStats0.numUserInsts")


def ruby_cache_accesses():
    accesses = 0
    for controller in (system.cpu.l1d, system.cpu.l1i, system.cpu.l2):
        cache_stats = get_simstat(controller)["cache"]
        accesses += int(cache_stats["m_demand_hits"].value)
        accesses += int(cache_stats["m_demand_misses"].value)
    return accesses


def ruby_memory_bytes():
    return stat_value("system.mem_ctrls.bytesReadSys") + stat_value(
        "system.mem_ctrls.bytesWrittenSys"
    )


def check_guest_log():
    terminal_path = os.path.join(m5.options.outdir, "system.platform.terminal")
    failure_markers = (
        "Kernel panic",
        "Oops:",
        "BUG:",
        "Unhandled fault",
        "Unable to handle kernel",
        "Instruction access fault",
        "Load access fault",
        "Store/AMO access fault",
    )

    with open(terminal_path, encoding="utf-8", errors="replace") as terminal:
        guest_log = terminal.read()

    detected = [marker for marker in failure_markers if marker in guest_log]
    if detected:
        raise RuntimeError(
            "guest kernel failure marker(s) found: " + ", ".join(detected)
        )
    print(f"Guest log check passed ({terminal_path})")


parser = argparse.ArgumentParser()
parser.add_argument("linux_image")
parser.add_argument("backend")
parser.add_argument("--cpu", choices=("jit", "noncaching"), default="jit")
parser.add_argument("--rtc-frequency", default="1MHz")
parser.add_argument("--max-ticks", type=int, default=2_000_000_000_000)
parser.add_argument("--switch-to-o3", action="store_true")
parser.add_argument("--o3-ticks", type=int, default=10_000_000)
parser.add_argument("--ruby-chi", action="store_true")
parser.add_argument(
    "--ruby-network", choices=("simple", "garnet"), default="simple"
)
parser.add_argument("--repeated-switches", type=int, default=0)
parser.add_argument("--phase-ticks", type=int, default=10_000_000)
parser.add_argument("--final-o3-ticks", type=int, default=50_000_000)
parser.add_argument("--initrd")
parser.add_argument(
    "--initrd-addr", type=lambda value: int(value, 0), default=0x88000000
)
args = parser.parse_args()

if args.ruby_chi and buildEnv["PROTOCOL"] != "CHI":
    parser.error("--ruby-chi requires a gem5 binary built with PROTOCOL=CHI")
if args.ruby_network != "simple" and not args.ruby_chi:
    parser.error("--ruby-network requires --ruby-chi")
if args.repeated_switches:
    if not args.ruby_chi:
        parser.error("--repeated-switches requires --ruby-chi")
    if args.repeated_switches < 3 or args.repeated_switches % 2 == 0:
        parser.error("--repeated-switches must be an odd value of at least 3")
    if not args.initrd:
        parser.error(
            "--repeated-switches requires the JitCPU Linux test initramfs"
        )
    args.switch_to_o3 = True

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
    ruby_args = ruby_options(args.ruby_network)
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
if args.initrd:
    system.workload = RiscvBootloaderKernelWorkload(
        bootloader_filename=args.linux_image,
        bootloader_addr=0x80000000,
        entry_point=0x80000000,
        dtb_filename=dtb_path,
        dtb_addr=0x87E00000,
        initrd_filename=args.initrd,
        initrd_addr=args.initrd_addr,
        command_line="console=ttyS0",
        exit_on_kernel_panic=False,
    )
else:
    system.workload = RiscvLinux(
        object_file=args.linux_image,
        dtb_filename=dtb_path,
        dtb_addr=0x87E00000,
        command_line="console=ttyS0",
        addr_check=False,
    )

root = Root(full_system=True, system=system)
m5.instantiate()


def check_memory_mode(cpu_name):
    expected = "atomic_noncaching" if cpu_name == "cpu" else "timing"
    memory_mode = params.allEnums["MemoryMode"]
    actual = system.getMemoryMode()
    if actual != memory_mode(expected).getValue():
        raise RuntimeError(
            f"{cpu_name} requires {expected} mode, found {actual}"
        )


def validate_linux_phase(cpu_name, phase_name):
    user_insts = cpu_user_instructions(cpu_name)
    print(f"{phase_name}: {cpu_name} committed {user_insts} user instructions")
    if user_insts == 0:
        raise RuntimeError(f"{phase_name}: no userspace instruction progress")

    if not args.ruby_chi:
        return

    ruby_messages = ruby_router_messages(system.ruby)
    print(f"{phase_name}: Ruby CHI carried {ruby_messages} messages")
    if cpu_name == "cpu":
        if ruby_messages != 0:
            raise RuntimeError(f"{phase_name}: JitCPU generated CHI traffic")
        return

    cache_accesses = ruby_cache_accesses()
    memory_bytes = ruby_memory_bytes()
    print(
        f"{phase_name}: CHI caches handled {cache_accesses} accesses; "
        f"memory handled {memory_bytes} bytes"
    )
    if ruby_messages == 0:
        raise RuntimeError(f"{phase_name}: O3CPU generated no CHI traffic")
    if cache_accesses == 0:
        raise RuntimeError(f"{phase_name}: O3CPU generated no cache accesses")
    if memory_bytes == 0:
        raise RuntimeError(f"{phase_name}: O3CPU generated no memory traffic")


exit_event = m5.simulate(args.max_ticks)
print(
    f"JitCPU Linux stopped @ tick {m5.curTick()}: " f"{exit_event.getCause()}"
)

if args.switch_to_o3:
    if exit_event.getCause() != "m5_exit instruction encountered":
        raise RuntimeError("Linux did not reach its userspace m5 exit")

    validate_linux_phase("cpu", "Linux boot phase")

    if args.repeated_switches:
        active_cpu = system.cpu
        active_name = "cpu"

        for switch_index in range(args.repeated_switches):
            if active_cpu is system.cpu:
                next_cpu = system.o3
                next_name = "o3"
            else:
                next_cpu = system.cpu
                next_name = "cpu"

            m5.switchCpus(
                system,
                [(active_cpu, next_cpu)],
                is_ruby=True,
            )
            active_cpu = next_cpu
            active_name = next_name
            check_memory_mode(active_name)
            print(
                f"Linux switch {switch_index + 1}/"
                f"{args.repeated_switches}: active {active_name}, "
                f"memory mode {system.getMemoryMode()}"
            )

            m5.stats.reset()
            interval = (
                args.final_o3_ticks
                if switch_index == args.repeated_switches - 1
                else args.phase_ticks
            )
            exit_event = m5.simulate(interval)
            print(
                f"Linux phase {switch_index + 1} stopped @ "
                f"tick {m5.curTick()}: {exit_event.getCause()}"
            )
            if exit_event.getCause() != "simulate() limit reached":
                raise RuntimeError(
                    "Linux did not remain alive for the bounded phase: "
                    f"{exit_event.getCause()}"
                )
            validate_linux_phase(
                active_name, f"Linux phase {switch_index + 1}"
            )

        if active_cpu is not system.o3:
            raise RuntimeError("Linux repeated switching did not finish on O3")
        check_guest_log()
        print(
            "Linux repeated-switch validation passed after "
            f"{args.repeated_switches} switches"
        )
        raise SystemExit(0)

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
        f"O3CPU continued @ tick {m5.curTick()}: " f"{exit_event.getCause()}"
    )
    if exit_event.getCause() != "simulate() limit reached":
        raise RuntimeError("O3CPU did not complete its validation interval")

    o3_user_insts = cpu_user_instructions("o3")
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
