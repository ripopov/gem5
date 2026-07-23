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
from jitcpu_16core_mesh import (
    add_16core_mesh_option,
    apply_16core_mesh_options,
    configure_ruby_options,
    validate_16core_build,
    validate_16core_mesh,
)


def ruby_options(network, num_cpus, num_dirs, num_l3caches, mesh_4x4):
    parser = argparse.ArgumentParser(add_help=False)
    Options.addCommonOptions(parser)
    Ruby.define_options(parser)
    options = parser.parse_args([])
    options.num_cpus = num_cpus
    options.num_dirs = num_dirs
    options.num_l3caches = num_l3caches
    options.topology = "Crossbar"
    options.network = network
    configure_ruby_options(options, mesh_4x4)
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


def cpu_instructions(cpu):
    leaf = "commitStats0.numInsts"
    try:
        return stat_value(f"{cpu.path()}.{leaf}")
    except KeyError:
        # SimObject vector paths are zero-padded at 10+ entries, while the
        # corresponding stat names are not. Use the stable CPU id spelling.
        collection = (
            "o3" if any(cpu is candidate for candidate in o3_cpus) else "cpu"
        )
        return stat_value(f"system.{collection}{int(cpu.cpu_id)}.{leaf}")


def stat_value(name):
    value = Root.getInstance().resolveStat(name).value
    if isinstance(value, list):
        return int(sum(value))
    return int(value)


def ruby_cache_accesses():
    accesses = []
    for cpu in jit_cpus:
        cpu_accesses = 0
        for controller in (cpu.l1d, cpu.l1i, cpu.l2):
            cache_stats = get_simstat(controller)["cache"]
            cpu_accesses += int(cache_stats["m_demand_hits"].value)
            cpu_accesses += int(cache_stats["m_demand_misses"].value)
        accesses.append(cpu_accesses)
    return accesses


def ruby_memory_bytes():
    return [
        stat_value(f"{controller.path()}.bytesReadSys")
        + stat_value(f"{controller.path()}.bytesWrittenSys")
        for controller in system.mem_ctrls
    ]


parser = argparse.ArgumentParser()
parser.add_argument("binary")
parser.add_argument("backend")
parser.add_argument("--max-ticks", type=int, default=100000)
parser.add_argument("--num-cpus", type=int, default=1)
parser.add_argument("--num-dirs", type=int, default=1)
parser.add_argument("--num-l3caches", type=int, default=1)
parser.add_argument("--switch-to-o3", action="store_true")
parser.add_argument("--ruby-chi", action="store_true")
parser.add_argument(
    "--ruby-network", choices=("simple", "garnet"), default="simple"
)
add_16core_mesh_option(parser)
parser.add_argument("--repeated-switches", type=int, default=0)
parser.add_argument(
    "--unsafe-skip-ruby-maintenance",
    action="store_true",
    help="test-only negative control for the O3-to-JitCPU switch",
)
args = parser.parse_args()
apply_16core_mesh_options(args)

if args.num_cpus < 1:
    parser.error("--num-cpus must be at least 1")
if args.num_dirs < 1 or args.num_dirs & (args.num_dirs - 1):
    parser.error("--num-dirs must be a power of two")
if args.num_l3caches < 1:
    parser.error("--num-l3caches must be at least 1")
if args.ruby_chi and buildEnv["PROTOCOL"] != "CHI":
    parser.error("--ruby-chi requires a gem5 binary built with PROTOCOL=CHI")
if args.chi_4x4_mesh:
    validate_16core_build(buildEnv)
if args.repeated_switches and not args.ruby_chi:
    parser.error("--repeated-switches requires --ruby-chi")
if args.ruby_network != "simple" and not args.ruby_chi:
    parser.error("--ruby-network requires --ruby-chi")
if args.unsafe_skip_ruby_maintenance and not args.repeated_switches:
    parser.error("--unsafe-skip-ruby-maintenance requires --repeated-switches")
if args.unsafe_skip_ruby_maintenance and args.num_cpus != 1:
    parser.error("--unsafe-skip-ruby-maintenance requires one CPU")
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
system.platform.setNumCores(args.num_cpus)

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

jit_cpus = [
    RiscvJitCPU(
        clk_domain=system.cpu_clk_domain,
        cpu_id=index,
        backend_path=args.backend,
        backend_instance=index,
        backend_instance_count=args.num_cpus,
        batch_size=256,
    )
    for index in range(args.num_cpus)
]
system.cpu = jit_cpus[0] if args.num_cpus == 1 else jit_cpus

uncacheable_range = [
    *system.platform._on_chip_ranges(),
    *system.platform._off_chip_ranges(),
]
o3_cpus = []
if args.switch_to_o3:
    o3_cpus = [
        RiscvO3CPU(
            clk_domain=system.cpu_clk_domain,
            cpu_id=index,
            switched_out=True,
        )
        for index in range(args.num_cpus)
    ]
    system.o3 = o3_cpus[0] if args.num_cpus == 1 else o3_cpus

for cpu in [*jit_cpus, *o3_cpus]:
    if any(cpu is jit_cpu for jit_cpu in jit_cpus) and not args.ruby_chi:
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
    cpu.mmu.pma_checker = PMAChecker(uncacheable=uncacheable_range)

if args.ruby_chi:
    ruby_args = ruby_options(
        args.ruby_network,
        args.num_cpus,
        args.num_dirs,
        args.num_l3caches,
        args.chi_4x4_mesh,
    )
    Ruby.create_system(
        ruby_args,
        True,
        system,
        system.iobus,
        dma_ports=[],
        bootmem=None,
        cpus=jit_cpus,
    )
    system.ruby.clk_domain = SrcClockDomain(
        clock=ruby_args.ruby_clock,
        voltage_domain=system.voltage_domain,
    )
    system.iobus.mem_side_ports = system.ruby._io_port.in_ports
    if len(system.ruby._cpu_ports) != args.num_cpus:
        raise RuntimeError(
            "CHI created an unexpected number of CPU sequencers: "
            f"{len(system.ruby._cpu_ports)}"
        )
    for ruby_port, cpu in zip(system.ruby._cpu_ports, jit_cpus):
        ruby_port.connectCpuPorts(cpu)
    if len(system.mem_ctrls) != args.num_dirs:
        raise RuntimeError(
            "CHI created an unexpected number of memory controllers: "
            f"{len(system.mem_ctrls)}"
        )
    print(
        f"Configured {args.num_cpus} CPU cores, {args.num_l3caches} HNFs, "
        f"and {len(system.mem_ctrls)} memory controllers"
    )
    if args.chi_4x4_mesh:
        validate_16core_mesh(system)
else:
    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR3_1600_8x8(range=system.mem_ranges[0])
    system.mem_ctrl.port = system.membus.mem_side_ports

root = Root(full_system=True, system=system)
m5.instantiate()


def validate_phase(cpus, cpu_name):
    instructions = [cpu_instructions(cpu) for cpu in cpus]
    print(
        f"{cpu_name} per-core retired instructions: "
        + ", ".join(str(value) for value in instructions)
    )
    stalled = [index for index, value in enumerate(instructions) if value == 0]
    if stalled:
        raise RuntimeError(
            f"{cpu_name} made no instruction progress on core(s) {stalled}"
        )

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
                f"{cpu_name} per-RNF CHI cache accesses: "
                + ", ".join(str(value) for value in cache_accesses)
            )
            print(
                f"{cpu_name} per-controller memory bytes: "
                + ", ".join(str(value) for value in memory_bytes)
            )
            idle_rnfs = [
                index
                for index, value in enumerate(cache_accesses)
                if value == 0
            ]
            if idle_rnfs:
                raise RuntimeError(
                    "O3CPU generated no CHI cache accesses on RNF(s) "
                    f"{idle_rnfs}"
                )
            idle_controllers = [
                index for index, value in enumerate(memory_bytes) if value == 0
            ]
            if idle_controllers:
                raise RuntimeError(
                    "O3CPU generated no traffic at memory controller(s) "
                    f"{idle_controllers}"
                )


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
    active_cpus = jit_cpus
    active_name = "cpu"
    for switch_index in range(args.repeated_switches):
        if exit_event.getCause() != "switchcpu":
            raise RuntimeError(
                f"phase {switch_index} did not execute m5_switch_cpu: "
                f"{exit_event.getCause()}"
            )
        validate_phase(active_cpus, active_name)

        if active_cpus is jit_cpus:
            next_cpus = o3_cpus
            next_name = "o3"
        else:
            next_cpus = jit_cpus
            next_name = "cpu"
        if args.unsafe_skip_ruby_maintenance and active_cpus is o3_cpus:
            unsafe_switch_without_ruby_maintenance(
                active_cpus[0], next_cpus[0]
            )
        else:
            m5.switchCpus(
                system,
                list(zip(active_cpus, next_cpus)),
                is_ruby=True,
            )
        active_cpus = next_cpus
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
    if active_cpus is not o3_cpus:
        raise RuntimeError("repeated-switch regression did not finish on O3")
    validate_phase(active_cpus, active_name)
    print(
        f"Repeated-switch regression passed @ tick {m5.curTick()} "
        f"after {args.repeated_switches} switches"
    )
elif args.switch_to_o3:
    if exit_event.getCause() != "switchcpu":
        raise RuntimeError("JitCPU did not execute m5_switch_cpu")
    validate_phase(jit_cpus, "cpu")
    m5.switchCpus(
        system,
        list(zip(jit_cpus, o3_cpus)),
        is_ruby=args.ruby_chi,
    )
    print(
        f"Switched JitCPU -> O3CPU @ tick {m5.curTick()}, "
        f"memory mode {system.getMemoryMode()}"
    )
    check_memory_mode("o3")
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
    validate_phase(o3_cpus, "o3")
