# SPDX-License-Identifier: BSD-3-Clause

"""Small full-system platform for the bare-metal multicore regressions."""

import argparse
from pathlib import Path

import m5
from m5.objects import (
    AddrRange,
    BadAddr,
    Cache,
    DDR4_2400_8x8,
    MemCtrl,
    PMAChecker,
    RiscvBareMetal,
    RiscvDirectMemorySimpleCPU,
    RiscvNonCachingSimpleCPU,
    RiscvSystem,
    RiscvTimingSimpleCPU,
    Root,
    SimpleMemory,
    SrcClockDomain,
    SystemXBar,
    VoltageDomain,
)
from m5.util import addToPath

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary")
parser.add_argument("--cores", type=int, choices=(2, 4, 8), default=2)
parser.add_argument("--channels", type=int, choices=(1, 2, 4), default=1)
parser.add_argument(
    "--cpu", choices=("direct", "noncaching", "mixed"), default="direct"
)
parser.add_argument(
    "--topology", choices=("flat", "classic", "ruby"), default="flat"
)
parser.add_argument("--memory", choices=("simple", "ddr4"), default="simple")
parser.add_argument("--switches", type=int, default=0)
parser.add_argument(
    "--switch-to", choices=("direct", "noncaching", "timing"), default="direct"
)
args = parser.parse_args()

system = RiscvSystem(
    mem_mode="atomic_noncaching",
    mem_ranges=[AddrRange(0x80000000, size="32MiB")],
    cache_line_size=64,
)
system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=system.voltage_domain
)
classes = {
    "direct": RiscvDirectMemorySimpleCPU,
    "noncaching": RiscvNonCachingSimpleCPU,
    "timing": RiscvTimingSimpleCPU,
}


def make_cpu(index, kind, switched_out=False):
    if kind == "mixed":
        kind = "noncaching" if index % 2 else "direct"
    cpu = classes[kind](cpu_id=index, switched_out=switched_out)
    cpu.createInterruptController()
    cpu.createThreads()
    cpu.isa[0].riscv_profile = "RVA23S64"
    cpu.isa[0].privilege_mode_set = "MSU"
    # Keep ROM operations at the memory owner, including in timing mode.
    cpu.mmu.pma_checker = PMAChecker(
        misaligned=system.mem_ranges,
        uncacheable=[AddrRange(0x90000000, size="4KiB")],
    )
    return cpu


system.cpu = [make_cpu(i, args.cpu) for i in range(args.cores)]
if args.switches:
    system.exit_on_work_items = True
    system.next_cpu = [
        make_cpu(i, args.switch_to, True) for i in range(args.cores)
    ]

memories, ports = [], []
bits = args.channels.bit_length() - 1
for channel in range(args.channels):
    region = system.mem_ranges[0]
    if bits:
        region = AddrRange(
            0x80000000,
            size="32MiB",
            intlvHighBit=5 + bits,
            intlvBits=bits,
            intlvMatch=channel,
        )
    if args.memory == "ddr4":
        mem = MemCtrl(
            dram=DDR4_2400_8x8(range=region, addr_mapping="RoRaBaCoCh")
        )
    else:
        mem = SimpleMemory(range=region, latency="0ns")
    memories.append(mem)
    ports.append((region, mem.port))
system.mem_ctrls = memories
system.rom = SimpleMemory(
    range=AddrRange(0x90000000, size="4KiB"), writeable=False
)

if args.topology == "ruby":
    # Use the same CHI builder as configs/example/riscv/noncaching_fs.py.
    addToPath(str(Path(__file__).resolve().parents[3] / "configs"))
    from ruby import CHI

    from m5.objects import (
        IOXBar,
        RubyPortProxy,
        RubySystem,
        SimpleExtLink,
        SimpleIntLink,
        SimpleNetwork,
        Switch,
    )

    options = argparse.Namespace(
        num_cpus=args.cores,
        num_dirs=args.channels,
        num_l3caches=1,
        l1i_size="16KiB",
        l1i_assoc=2,
        l1d_size="16KiB",
        l1d_assoc=2,
        l2_size="64KiB",
        l2_assoc=4,
        l3_size="256KiB",
        l3_assoc=8,
        cacheline_size=64,
        enable_dvm=False,
        chi_config=None,
        network="simple",
        topology="Crossbar",
        simple_physical_channels=False,
        link_latency=1,
        router_latency=1,
    )
    system.ruby = RubySystem(
        block_size_bytes=64, memory_size_bits=48, access_backing_store=False
    )
    system.ruby.network = SimpleNetwork(
        ruby_system=system.ruby, topology="Crossbar", netifs=[]
    )
    seqs, dirs, topology = CHI.create_system(
        options, True, system, [], system.rom, system.ruby, system.cpu
    )
    topology.makeTopology(
        options, system.ruby.network, SimpleIntLink, SimpleExtLink, Switch
    )
    system.ruby.network.setup_buffers()
    system.ruby.num_of_sequencers = 2 * args.cores + 1
    for directory, (region, port) in zip(dirs, ports):
        directory.addr_ranges = [region]
        directory.memory_out_port = port
    system.iobus = IOXBar()
    system.iobus.badaddr = BadAddr()
    system.iobus.default = system.iobus.badaddr.pio
    for seq, cpu in zip(seqs, system.cpu):
        seq.connectCpuPorts(cpu)
        seq.connectIOPorts(system.iobus)
    system.sys_port_proxy = RubyPortProxy(ruby_system=system.ruby)
    system.sys_port_proxy.pio_request_port = system.iobus.cpu_side_ports
    system.system_port = system.sys_port_proxy.in_ports
else:
    system.membus = SystemXBar()
    system.system_port = system.membus.cpu_side_ports
    for _, port in ports:
        system.membus.mem_side_ports = port
    system.membus.mem_side_ports = system.rom.port
    for cpu in system.cpu:
        if args.topology == "classic":

            def cache():
                return Cache(
                    size="16KiB",
                    assoc=2,
                    tag_latency=1,
                    data_latency=1,
                    response_latency=1,
                    mshrs=4,
                    tgts_per_mshr=8,
                )

            cpu.icache = cache()
            cpu.icache.is_read_only = True
            cpu.icache.writeback_clean = True
            cpu.dcache = cache()
            cpu.icache_port = cpu.icache.cpu_side
            cpu.dcache_port = cpu.dcache.cpu_side
            cpu.icache.mem_side = system.membus.cpu_side_ports
            cpu.dcache.mem_side = system.membus.cpu_side_ports
        else:
            cpu.icache_port = system.membus.cpu_side_ports
            cpu.dcache_port = system.membus.cpu_side_ports
        cpu.mmu.connectWalkerPorts(
            system.membus.cpu_side_ports, system.membus.cpu_side_ports
        )

system.workload = RiscvBareMetal(bootloader=args.binary)
root = Root(full_system=True, system=system)
m5.instantiate()
current, replacement = system.cpu, getattr(system, "next_cpu", None)
for _ in range(args.switches):
    event = m5.simulate(10**10)
    if event.getCause() != "workbegin":
        raise RuntimeError(
            f"Expected switch marker: {event.getCause()}, "
            f"code={event.getCode()}"
        )
    m5.switchCpus(system, list(zip(current, replacement)))
    current, replacement = replacement, current

event = m5.simulate(10**10)
print(f"RESULT: {event.getCause()}, code={event.getCode()}")
if event.getCause() != "m5_exit instruction encountered" or event.getCode():
    raise SystemExit(1)
print("MULTICORE PASS")
