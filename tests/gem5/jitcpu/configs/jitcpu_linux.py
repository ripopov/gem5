# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause

import argparse
import os
import re

import m5
from m5 import params
from m5.defines import buildEnv
from m5.objects import (
    AddrRange,
    BadAddr,
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
from m5.objects.RiscvCPU import (
    RiscvNonCachingSimpleCPU,
    RiscvO3CPU,
)
from m5.util import addToPath
from m5.util.convert import toFrequency
from m5.util.fdthelper import (
    Fdt,
    FdtNode,
    FdtPropertyStrings,
    FdtPropertyWords,
    FdtState,
)

addToPath(os.path.join(m5.util.repoPath(), "configs"))

from jitcpu_16core_mesh import (
    add_16core_mesh_option,
    apply_16core_mesh_options,
    validate_16core_build,
    validate_16core_mesh,
)
from jitcpu_common import (
    cpu_stat,
    ruby_cache_accesses,
    ruby_memory_bytes,
    ruby_options,
    ruby_router_messages,
)
from ruby import Ruby


def generate_dtb(system, cpus, output):
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

    for section in [*cpus, system.platform]:
        for node in section.generateDeviceTree(state):
            if node.get_name() == root.get_name():
                root.merge(node)
            else:
                root.append(node)

    fdt = Fdt()
    fdt.add_rootnode(root)
    fdt.writeDtbFile(output)


def read_guest_log():
    terminal_path = os.path.join(m5.options.outdir, "system.platform.terminal")
    with open(terminal_path, encoding="utf-8", errors="replace") as terminal:
        return terminal_path, terminal.read()


def check_guest_log(required_markers=()):
    terminal_path, guest_log = read_guest_log()
    failure_markers = (
        "Kernel panic",
        "Oops:",
        "BUG:",
        "Unhandled fault",
        "Unable to handle kernel",
        "Instruction access fault",
        "Load access fault",
        "Store/AMO access fault",
        "Couldn't find cpu id",
        "failed to come online",
        "Failed to bring up CPU",
        "Illegal instruction",
        "JITCPU-DINING FAIL",
    )

    detected = [marker for marker in failure_markers if marker in guest_log]
    if detected:
        raise RuntimeError(
            "guest kernel failure marker(s) found: " + ", ".join(detected)
        )
    missing = [
        marker for marker in required_markers if marker not in guest_log
    ]
    if missing:
        raise RuntimeError(
            "guest workload marker(s) missing: " + ", ".join(missing)
        )
    print(f"Guest log check passed ({terminal_path})")


def validate_dining_guest_phase(phase, cpu_model):
    check_guest_log(
        (
            "JITCPU-DINING ONLINE cpus=16",
            "JITCPU-DINING READY",
            f"JITCPU-DINING PHASE-PASS phase={phase} model={cpu_model}",
        )
    )
    _, guest_log = read_guest_log()
    reports = [
        (philosopher, cpu)
        for philosopher, cpu, report_phase in re.findall(
            r"JITCPU-DINING PHASE philosopher=(\d+) cpu=(\d+) " r"phase=(\d+)",
            guest_log,
        )
        if int(report_phase) == phase
    ]
    expected_pairs = [(str(index), str(index)) for index in range(16)]
    if sorted(reports) != sorted(expected_pairs):
        raise RuntimeError(
            f"guest phase {phase} reports are {sorted(reports)}, "
            f"expected {sorted(expected_pairs)}"
        )

    if phase != 0:
        return

    online = re.findall(r"JITCPU-DINING ONLINE cpus=(\d+)", guest_log)
    affinity = re.findall(
        r"JITCPU-DINING AFFINITY philosopher=(\d+) cpu=(\d+)",
        guest_log,
    )
    if online != ["16"]:
        raise RuntimeError(f"guest online-CPU report is {online}, expected 16")
    if sorted(affinity) != sorted(expected_pairs):
        raise RuntimeError(
            f"guest affinity reports are {sorted(affinity)}, "
            f"expected {sorted(expected_pairs)}"
        )


parser = argparse.ArgumentParser()
parser.add_argument("linux_image")
parser.add_argument("backend")
parser.add_argument(
    "--kernel",
    help=(
        "separate RISC-V Linux ELF loaded at 0x80200000; linux_image is "
        "then the OpenSBI bootloader"
    ),
)
parser.add_argument("--cpu", choices=("jit", "noncaching"), default="jit")
parser.add_argument("--num-cpus", type=int, default=1)
parser.add_argument("--num-dirs", type=int, default=1)
parser.add_argument("--num-l3caches", type=int, default=1)
parser.add_argument("--rtc-frequency", default="1MHz")
parser.add_argument(
    "--batch-size",
    type=int,
    default=65536,
    help=(
        "maximum translated guest instructions per JitCPU event; with the "
        "lazy CLINT the platform no longer bounds batches at one RTC period"
    ),
)
parser.add_argument(
    "--classic-rtc-events",
    action="store_true",
    help=(
        "drive the CLINT with one RTC pin event per mtime tick instead of "
        "the lazy deadline-scheduled timer"
    ),
)
parser.add_argument("--max-ticks", type=int, default=2_000_000_000_000)
parser.add_argument("--switch-to-o3", action="store_true")
parser.add_argument("--o3-ticks", type=int, default=10_000_000)
parser.add_argument("--ruby-chi", action="store_true")
parser.add_argument(
    "--ruby-network", choices=("simple", "garnet"), default="simple"
)
add_16core_mesh_option(parser)
parser.add_argument("--repeated-switches", type=int, default=0)
parser.add_argument("--phase-ticks", type=int, default=10_000_000)
parser.add_argument("--final-o3-ticks", type=int, default=50_000_000)
parser.add_argument("--initrd")
parser.add_argument(
    "--initrd-addr", type=lambda value: int(value, 0), default=0x88000000
)
parser.add_argument(
    "--workload-handoff",
    action="store_true",
    help="run a guest-coordinated one-way JitCPU-to-O3 workload handoff",
)
parser.add_argument(
    "--workload-switches",
    type=int,
    default=0,
    help=(
        "perform this many guest-coordinated 16-core mesh switches; the "
        "userspace workload must request and validate every phase"
    ),
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
    if not args.kernel:
        parser.error(
            "--chi-4x4-mesh requires --kernel so the 16-hart-capable Linux "
            "kernel is separate from the OpenSBI bootloader"
        )
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
if args.workload_handoff:
    if args.workload_switches:
        parser.error(
            "--workload-handoff cannot be combined with --workload-switches"
        )
    args.workload_switches = 1
if args.workload_switches:
    if not args.chi_4x4_mesh:
        parser.error("--workload-switches requires --chi-4x4-mesh")
    if args.repeated_switches:
        parser.error(
            "--workload-switches cannot be combined with "
            "--repeated-switches"
        )
    if not args.workload_handoff and (
        args.workload_switches < 3 or args.workload_switches % 2 == 0
    ):
        parser.error("--workload-switches must be an odd value of at least 3")
    if not args.initrd:
        parser.error("--workload-switches requires --initrd")
    args.switch_to_o3 = True

system = RiscvSystem()
system.mem_mode = "atomic_noncaching"
system.mem_ranges = [
    AddrRange(
        start=0x80000000,
        size="1GiB" if args.chi_4x4_mesh else "256MiB",
    )
]

system.iobus = IOXBar()
# O3 issues wrong-path speculative accesses to unmapped addresses;
# without a default responder the crossbar treats them as fatal.
# This mirrors gem5's own RiscvBoard.
system.iobus.badaddr_responder = BadAddr()
system.iobus.default = system.iobus.badaddr_responder.pio
if not args.ruby_chi:
    system.membus = SystemXBar()
    system.system_port = system.membus.cpu_side_ports

system.platform = HiFive()
if args.classic_rtc_events:
    system.platform.rtc = RiscvRTC(frequency=args.rtc_frequency)
    system.platform.clint.int_pin = system.platform.rtc.int_pin
else:
    # Lazy CLINT: mtime advances at the RTC frequency but is computed from
    # curTick on demand; the only timer events are mtimecmp deadlines. This
    # removes the one-event-per-microsecond cadence that capped every JitCPU
    # batch at ~1000 instructions.
    system.platform.clint.rtc_period = (
        f"{1.0 / toFrequency(args.rtc_frequency)}s"
    )
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
    jit_cpus = [
        RiscvJitCPU(
            clk_domain=system.cpu_clk_domain,
            cpu_id=index,
            backend_path=args.backend,
            backend_instance=index,
            backend_instance_count=args.num_cpus,
            batch_size=args.batch_size,
        )
        for index in range(args.num_cpus)
    ]
else:
    jit_cpus = [
        RiscvNonCachingSimpleCPU(
            clk_domain=system.cpu_clk_domain,
            cpu_id=index,
        )
        for index in range(args.num_cpus)
    ]

system.cpu = jit_cpus[0] if args.num_cpus == 1 else jit_cpus

o3_cpus = []
if args.switch_to_o3:
    if args.cpu != "jit":
        parser.error("--switch-to-o3 requires --cpu jit")
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
    cpu.mmu.pma_checker = PMAChecker(
        uncacheable=[
            *system.platform._on_chip_ranges(),
            *system.platform._off_chip_ranges(),
        ]
    )

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

dtb_path = os.path.join(m5.options.outdir, "device.dtb")
generate_dtb(system, jit_cpus, dtb_path)
if args.initrd:
    system.workload = RiscvBootloaderKernelWorkload(
        bootloader_filename=args.linux_image,
        bootloader_addr=0x80000000,
        object_file=args.kernel or "",
        kernel_addr=0x80200000,
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


def check_memory_mode(cpu_model):
    expected = "atomic_noncaching" if cpu_model == "jit" else "timing"
    memory_mode = params.allEnums["MemoryMode"]
    actual = system.getMemoryMode()
    if actual != memory_mode(expected).getValue():
        raise RuntimeError(
            f"{cpu_model} requires {expected} mode, found {actual}"
        )


def validate_linux_phase(cpus, cpu_model, phase_name):
    if cpu_model == "jit":
        # JitCPU attributes privilege at batch boundaries, which is
        # documented as approximate: batches preferentially end at MMIO and
        # interrupts, so with large batches a core's entire userspace
        # quantum can be charged to kernel mode. Require per-core execution
        # progress here; per-worker userspace progress is proven by the
        # guest-side terminal markers instead.
        leaf = "commitStats0.numInsts"
        label = "instructions"
        failure = "no progress"
    else:
        leaf = "commitStats0.numUserInsts"
        label = "user instructions"
        failure = "no userspace progress"
    insts = [cpu_stat(cpu, leaf, o3_cpus) for cpu in cpus]
    print(
        f"{phase_name}: {cpu_model} per-core {label} "
        + ", ".join(str(value) for value in insts)
    )
    stalled = [index for index, value in enumerate(insts) if value == 0]
    if stalled:
        raise RuntimeError(f"{phase_name}: {failure} on core(s) {stalled}")

    if not args.ruby_chi:
        return

    ruby_messages = ruby_router_messages(system.ruby, args.ruby_network)
    cache_accesses = ruby_cache_accesses(jit_cpus)
    memory_bytes = ruby_memory_bytes(system.mem_ctrls)
    if ruby_messages is not None:
        print(f"{phase_name}: Ruby CHI carried {ruby_messages} messages")
    print(
        f"{phase_name}: per-RNF CHI cache accesses "
        + ", ".join(str(value) for value in cache_accesses)
    )
    print(
        f"{phase_name}: per-controller memory bytes "
        + ", ".join(str(value) for value in memory_bytes)
    )

    if cpu_model == "jit":
        # JitCPU executes uncached. Nothing may reach the timing hierarchy,
        # on any network, from any requester. The controller-side counts
        # keep this check meaningful when the network cannot report totals.
        if ruby_messages:
            raise RuntimeError(f"{phase_name}: JitCPU generated CHI traffic")
        busy_rnfs = [
            index for index, value in enumerate(cache_accesses) if value != 0
        ]
        if busy_rnfs:
            raise RuntimeError(
                f"{phase_name}: JitCPU generated CHI cache accesses on "
                f"RNF(s) {busy_rnfs}"
            )
        busy_controllers = [
            index for index, value in enumerate(memory_bytes) if value != 0
        ]
        if busy_controllers:
            raise RuntimeError(
                f"{phase_name}: JitCPU generated memory traffic at "
                f"controller(s) {busy_controllers}"
            )
        return

    if ruby_messages == 0:
        raise RuntimeError(f"{phase_name}: O3CPU generated no CHI traffic")
    idle_rnfs = [
        index for index, value in enumerate(cache_accesses) if value == 0
    ]
    if idle_rnfs:
        raise RuntimeError(
            f"{phase_name}: no CHI cache accesses on RNF(s) {idle_rnfs}"
        )
    idle_controllers = [
        index for index, value in enumerate(memory_bytes) if value == 0
    ]
    if idle_controllers:
        raise RuntimeError(
            f"{phase_name}: no memory traffic at controller(s) "
            f"{idle_controllers}"
        )


exit_event = m5.simulate(args.max_ticks)
print(
    f"JitCPU Linux stopped @ tick {m5.curTick()}: " f"{exit_event.getCause()}"
)

if args.switch_to_o3:
    if exit_event.getCause() != "m5_exit instruction encountered":
        raise RuntimeError("Linux did not reach its userspace m5 exit")

    validate_linux_phase(jit_cpus, "jit", "Linux boot phase")

    if args.workload_switches:
        validate_dining_guest_phase(0, "jit")
        active_cpus = jit_cpus
        active_model = "jit"

        for switch_index in range(args.workload_switches):
            if active_model == "jit":
                next_cpus = o3_cpus
                next_model = "o3"
            else:
                next_cpus = jit_cpus
                next_model = "jit"

            m5.switchCpus(
                system,
                list(zip(active_cpus, next_cpus)),
                is_ruby=True,
            )
            active_cpus = next_cpus
            active_model = next_model
            check_memory_mode(active_model)
            print(
                f"Dining philosophers switch {switch_index + 1}/"
                f"{args.workload_switches}: active {active_model}, "
                f"memory mode {system.getMemoryMode()} @ tick {m5.curTick()}"
            )

            m5.stats.reset()
            exit_event = m5.simulate(args.o3_ticks)
            print(
                f"Dining philosophers phase {switch_index + 1} stopped @ "
                f"tick {m5.curTick()}: {exit_event.getCause()}"
            )
            if exit_event.getCause() != "m5_exit instruction encountered":
                raise RuntimeError(
                    f"dining philosophers phase {switch_index + 1} did not "
                    f"finish before the {args.o3_ticks}-tick timeout"
                )
            validate_linux_phase(
                active_cpus,
                active_model,
                f"Dining philosophers phase {switch_index + 1}",
            )
            check_memory_mode(active_model)
            validate_dining_guest_phase(switch_index + 1, active_model)

        if active_model != "o3":
            raise RuntimeError(
                "dining philosophers repeated switching did not finish on O3"
            )
        check_guest_log(
            (
                f"JITCPU-DINING PASS workers=16 "
                f"switches={args.workload_switches}",
            )
        )
        if args.workload_switches == 1:
            print(
                "16-core Linux dining-philosophers handoff passed on O3 in "
                f"timing mode @ tick {m5.curTick()}"
            )
        else:
            print(
                "16-core Linux dining-philosophers repeated-switch "
                f"validation passed after {args.workload_switches} switches "
                f"@ tick {m5.curTick()}"
            )
        raise SystemExit(0)

    if args.repeated_switches:
        active_cpus = jit_cpus
        active_model = "jit"

        for switch_index in range(args.repeated_switches):
            if active_model == "jit":
                next_cpus = o3_cpus
                next_model = "o3"
            else:
                next_cpus = jit_cpus
                next_model = "jit"

            m5.switchCpus(
                system,
                list(zip(active_cpus, next_cpus)),
                is_ruby=True,
            )
            active_cpus = next_cpus
            active_model = next_model
            check_memory_mode(active_model)
            print(
                f"Linux switch {switch_index + 1}/"
                f"{args.repeated_switches}: active {active_model}, "
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
                active_cpus,
                active_model,
                f"Linux phase {switch_index + 1}",
            )

        if active_model != "o3":
            raise RuntimeError("Linux repeated switching did not finish on O3")
        check_guest_log()
        print(
            "Linux repeated-switch validation passed after "
            f"{args.repeated_switches} switches"
        )
        raise SystemExit(0)

    m5.switchCpus(
        system,
        list(zip(jit_cpus, o3_cpus)),
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

    validate_linux_phase(o3_cpus, "o3", "O3 takeover phase")
