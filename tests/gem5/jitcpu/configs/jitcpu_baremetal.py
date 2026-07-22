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
    RiscvBareMetal,
    RiscvJitCPU,
    RiscvRTC,
    RiscvSystem,
    Root,
    SrcClockDomain,
    SystemXBar,
    VoltageDomain,
)
from m5.stats.gem5stats import get_simstat
from m5.util import addToPath
from m5.util.convert import toMemorySize
from m5.objects.RiscvCPU import RiscvO3CPU

addToPath(os.path.join(m5.util.repoPath(), "configs"))

from common import Options
from ruby import Ruby


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


def cpu_instructions(cpu_name):
    return stat_value(f"system.{cpu_name}.commitStats0.numInsts")


def stat_value(name):
    value = Root.getInstance().resolveStat(name).value
    if isinstance(value, list):
        return int(sum(value))
    return int(value)


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


parser = argparse.ArgumentParser()
parser.add_argument("binary")
parser.add_argument("backend")
parser.add_argument("--max-ticks", type=int, default=100000)
parser.add_argument("--switch-to-o3", action="store_true")
parser.add_argument("--ruby-chi", action="store_true")
parser.add_argument(
    "--ruby-network", choices=("simple", "garnet"), default="simple"
)
parser.add_argument("--repeated-switches", type=int, default=0)
parser.add_argument(
    "--unsafe-skip-ruby-maintenance",
    action="store_true",
    help="test-only negative control for the O3-to-JitCPU switch",
)
args = parser.parse_args()

if args.ruby_chi and buildEnv["PROTOCOL"] != "CHI":
    parser.error("--ruby-chi requires a gem5 binary built with PROTOCOL=CHI")
if args.repeated_switches and not args.ruby_chi:
    parser.error("--repeated-switches requires --ruby-chi")
if args.ruby_network != "simple" and not args.ruby_chi:
    parser.error("--ruby-network requires --ruby-chi")
if args.unsafe_skip_ruby_maintenance and not args.repeated_switches:
    parser.error("--unsafe-skip-ruby-maintenance requires --repeated-switches")
if args.repeated_switches:
    args.switch_to_o3 = True

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
if not args.ruby_chi:
    system.membus = SystemXBar()
    system.system_port = system.membus.cpu_side_ports

system.platform = HiFive()
system.platform.rtc = RiscvRTC(frequency="100MHz")
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
if not args.ruby_chi:
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
    system.o3.mmu.pma_checker = PMAChecker(uncacheable=uncacheable_range)

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

root = Root(full_system=True, system=system)
m5.instantiate()


def validate_phase(cpu_name):
    instructions = cpu_instructions(cpu_name)
    print(f"{cpu_name} retired {instructions} instructions in this phase")
    if instructions == 0:
        raise RuntimeError(f"{cpu_name} made no instruction progress")

    if args.ruby_chi:
        messages = ruby_router_messages(system.ruby)
        print(f"{cpu_name} generated {messages} timing CHI messages")
        if cpu_name == "cpu" and messages != 0:
            raise RuntimeError("JitCPU generated timing CHI traffic")
        if cpu_name == "o3" and messages == 0:
            raise RuntimeError("O3CPU generated no timing CHI traffic")
        if cpu_name == "o3":
            cache_accesses = ruby_cache_accesses()
            memory_bytes = ruby_memory_bytes()
            print(
                f"{cpu_name} generated {cache_accesses} CHI cache accesses "
                f"and {memory_bytes} memory bytes"
            )
            if cache_accesses == 0:
                raise RuntimeError("O3CPU generated no CHI cache accesses")
            if memory_bytes == 0:
                raise RuntimeError("O3CPU generated no memory traffic")


def check_memory_mode(cpu_name):
    expected = "atomic_noncaching" if cpu_name == "cpu" else "timing"
    memory_mode = params.allEnums["MemoryMode"]
    actual = system.getMemoryMode()
    expected_value = memory_mode(expected).getValue()
    if actual != expected_value:
        raise RuntimeError(
            f"{cpu_name} requires {expected} mode, found {actual}"
        )


def unsafe_switch_without_ruby_maintenance(old_cpu, new_cpu):
    """Deliberately violate coherence for the regression negative control."""
    print("UNSAFE negative control: skipping Ruby writeback/invalidation")
    m5.drain()
    old_cpu.switchOut()
    memory_mode = params.allEnums["MemoryMode"]
    system.setMemoryMode(memory_mode("atomic_noncaching").getValue())
    new_cpu.takeOverFrom(old_cpu)


exit_event = m5.simulate(args.max_ticks)
print(
    f"JitCPU smoke stopped @ tick {m5.curTick()}: "
    f"{exit_event.getCause()} (code {exit_event.getCode()})"
)

if args.repeated_switches:
    active_cpu = system.cpu
    active_name = "cpu"
    for switch_index in range(args.repeated_switches):
        if exit_event.getCause() != "switchcpu":
            raise RuntimeError(
                f"phase {switch_index} did not execute m5_switch_cpu: "
                f"{exit_event.getCause()}"
            )
        validate_phase(active_name)

        if active_cpu is system.cpu:
            next_cpu = system.o3
            next_name = "o3"
        else:
            next_cpu = system.cpu
            next_name = "cpu"
        if args.unsafe_skip_ruby_maintenance and active_cpu is system.o3:
            unsafe_switch_without_ruby_maintenance(active_cpu, next_cpu)
        else:
            m5.switchCpus(
                system,
                [(active_cpu, next_cpu)],
                is_ruby=True,
            )
        active_cpu = next_cpu
        active_name = next_name
        check_memory_mode(active_name)
        print(
            f"Completed switch {switch_index + 1}/"
            f"{args.repeated_switches}: active {active_name}, "
            f"memory mode {system.getMemoryMode()}"
        )
        m5.stats.reset()
        exit_event = m5.simulate(args.max_ticks)
        print(
            f"Phase {switch_index + 1} stopped @ tick {m5.curTick()}: "
            f"{exit_event.getCause()} (code {exit_event.getCode()})"
        )

    if exit_event.getCause() != "m5_exit instruction encountered":
        raise RuntimeError(
            "repeated-switch payload did not complete: "
            f"{exit_event.getCause()}"
        )
    if active_cpu is not system.o3:
        raise RuntimeError("repeated-switch regression did not finish on O3")
    validate_phase(active_name)
    print(
        f"Repeated-switch regression passed @ tick {m5.curTick()} "
        f"after {args.repeated_switches} switches"
    )
elif args.switch_to_o3:
    if exit_event.getCause() != "switchcpu":
        raise RuntimeError("JitCPU did not execute m5_switch_cpu")
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
    exit_event = m5.simulate(args.max_ticks)
    print(
        f"O3CPU switch smoke stopped @ tick {m5.curTick()}: "
        f"{exit_event.getCause()}"
    )
    if exit_event.getCause() != "m5_exit instruction encountered":
        raise RuntimeError(
            "O3CPU did not complete the S-mode PMP takeover regression"
        )
