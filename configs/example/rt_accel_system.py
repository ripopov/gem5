# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
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
"""Shared bare-metal accelerator wiring for the rendering/reduction examples.

Separate RiscvSystems and explicit event queues/transport are not expressible
through the stdlib boards. Keep this helper local to these examples.
"""

import m5
from m5.objects import (
    AddrRange,
    Cache,
    CoherentXBar,
    NoncoherentXBar,
    PMAChecker,
    RiscvBareMetal,
    RiscvO3CPU,
    RiscvSystem,
    RiscvTimingSimpleCPU,
    Root,
    RTAccelCodeTCM,
    RTAccelControl,
    RTAccelHostAdapter,
    RTAccelMSI,
    RTGem5Accelerator,
    SimpleMemory,
    SrcClockDomain,
    VoltageDomain,
)


def build_system(
    args,
    firmware,
    kernel,
    outdir,
    parameters,
    memory_latency="30ns",
    memory_bandwidth="12.8GiB/s",
    memory_width=16,
    memory_router_clock=None,
    memory_controllers=1,
    memory_range=None,
):
    if memory_controllers not in (1, 2, 4):
        raise ValueError("memory_controllers must be 1, 2 or 4")
    system = RiscvSystem(
        mem_mode="timing",
        readfile=str(parameters),
        mem_ranges=[AddrRange(0x80000000, size="64MiB")],
    )
    system.voltage_domain = VoltageDomain()
    system.clk_domain = SrcClockDomain(
        clock="1GHz", voltage_domain=system.voltage_domain
    )
    system.workload = RiscvBareMetal(bootloader=str(firmware))
    system.cpu = RiscvO3CPU(numThreads=1)
    system.cpu.mmu.pma_checker = PMAChecker(
        uncacheable=[
            AddrRange(0x10000000, size=0x1000 * args.accelerator_cores),
            AddrRange(0x20000000, size=0x1000),
            AddrRange(0x81000000, size="1MiB"),
        ]
    )
    system.cpu.createThreads()
    system.cpu.createInterruptController()
    # Multiplex instruction/data requests into the single unified host cache.
    system.cpu_bus = NoncoherentXBar(
        frontend_latency=0, forward_latency=0, response_latency=0, width=32
    )
    system.host_cache = Cache(
        size="64KiB",
        assoc=8,
        tag_latency=1,
        data_latency=1,
        response_latency=1,
        mshrs=16,
        tgts_per_mshr=16,
        writeback_clean=True,
    )
    system.cpu.icache_port = system.cpu_bus.cpu_side_ports
    system.cpu.dcache_port = system.cpu_bus.cpu_side_ports
    system.cpu_bus.mem_side_ports = system.host_cache.cpu_side
    system.membus = NoncoherentXBar(
        frontend_latency=1,
        forward_latency=1,
        response_latency=1,
        width=memory_width,
    )
    if memory_router_clock:
        system.memory_router_clock = SrcClockDomain(
            clock=memory_router_clock, voltage_domain=system.voltage_domain
        )
        system.membus.clk_domain = system.memory_router_clock
    system.host_cache.mem_side = system.membus.cpu_side_ports
    system.cpu.mmu.connectWalkerPorts(
        system.membus.cpu_side_ports, system.membus.cpu_side_ports
    )
    storage = (
        memory_range if memory_range is not None else system.mem_ranges[0]
    )
    interleave_bits = memory_controllers.bit_length() - 1
    ranges = (
        [storage]
        if memory_controllers == 1
        else [
            AddrRange(
                storage.start,
                size=storage.size(),
                intlvHighBit=5 + interleave_bits,
                intlvBits=interleave_bits,
                intlvMatch=i,
            )
            for i in range(memory_controllers)
        ]
    )
    system.memory = SimpleMemory(
        range=ranges[0],
        latency=memory_latency,
        bandwidth=memory_bandwidth,
    )
    memories = [system.memory]
    for i in range(1, memory_controllers):
        memory = SimpleMemory(
            range=ranges[i],
            latency=memory_latency,
            bandwidth=memory_bandwidth,
        )
        setattr(system, f"memory{i}", memory)
        memories.append(memory)
    system.memories = memories
    for memory in system.memories:
        memory.port = system.membus.mem_side_ports
    system.system_port = system.membus.cpu_side_ports
    system.adapters = [
        RTAccelHostAdapter(
            mmio_range=AddrRange(0x10000000 + i * 0x1000, size=0x1000),
            queue_depth=min(2, args.queue_depth),
            trace_file=str(outdir / f"host{i}.trace") if args.trace else "",
        )
        for i in range(args.accelerator_cores)
    ]
    system.msi = RTAccelMSI(
        cpu=system.cpu,
        accelerator_cores=args.accelerator_cores,
        registers=AddrRange(0x20000000, size=0x1000),
        trace_file=str(outdir / "msi.trace") if args.trace else "",
    )
    system.msi.pio = system.membus.mem_side_ports
    system.accelerators = [
        RiscvSystem(
            mem_mode="timing",
            mem_ranges=[
                AddrRange(0x40000000, size="64KiB"),
                AddrRange(0x40010000, size="192KiB"),
            ],
            eventq_index=i + 1 if args.queues != 1 else 0,
        )
        for i in range(args.accelerator_cores)
    ]
    for i, accel in enumerate(system.accelerators):
        accel.voltage_domain = VoltageDomain()
        accel.clk_domain = SrcClockDomain(
            clock="1GHz", voltage_domain=accel.voltage_domain
        )
        accel.workload = RiscvBareMetal(bootloader=str(kernel))
        cpu_class = (
            RiscvO3CPU
            if args.accelerator_cpu == "o3"
            else RiscvTimingSimpleCPU
        )
        accel.cpu = cpu_class(numThreads=1)
        accel.cpu.createThreads()
        accel.cpu.isa[0].riscv_profile = "RVA20S64"
        accel.cpu.isa[0].extra_extensions = []
        accel.cpu.createInterruptController()
        accel.cpu.mmu.pma_checker = PMAChecker(
            uncacheable=[
                AddrRange(0x10000000, size=0x1000),
                AddrRange(0x20000000, size=0x1000),
                system.mem_ranges[0],
            ]
        )
        cached = args.accelerator_caches == "l1"
        accel.router_clock = SrcClockDomain(
            clock="4GHz", voltage_domain=accel.voltage_domain
        )
        bus_class = CoherentXBar if cached else NoncoherentXBar
        coherence = (
            dict(
                snoop_response_latency=0,
                point_of_coherency=True,
                point_of_unification=True,
                max_routing_table_size=2048,
            )
            if cached
            else {}
        )
        accel.bus = bus_class(
            frontend_latency=0,
            forward_latency=0,
            response_latency=0,
            width=64,
            clk_domain=accel.router_clock,
            **coherence,
        )
        accel.code_tcm = RTAccelCodeTCM(range=accel.mem_ranges[0])
        accel.data_tcm = SimpleMemory(
            range=accel.mem_ranges[1], latency="1ns", bandwidth="128GiB/s"
        )
        accel.code_tcm.constants = accel.bus.mem_side_ports
        accel.data_tcm.port = accel.bus.mem_side_ports
        accel.memories = [accel.code_tcm, accel.data_tcm]
        if cached:
            accel.icache = Cache(
                size="8KiB",
                assoc=2,
                tag_latency=1,
                data_latency=1,
                response_latency=1,
                mshrs=4,
                tgts_per_mshr=16,
                is_read_only=True,
            )
            accel.dcache = Cache(
                addr_ranges=accel.mem_ranges,
                size="16KiB",
                assoc=4,
                tag_latency=1,
                data_latency=1,
                response_latency=1,
                mshrs=16,
                tgts_per_mshr=32,
                write_buffers=16,
            )
            accel.cpu.icache_port = accel.icache.cpu_side
            accel.icache.mem_side = accel.code_tcm.instruction
            accel.cpu.dcache_port = accel.dcache.cpu_side
            accel.dcache.mem_side = accel.bus.cpu_side_ports
        else:
            accel.cpu.icache_port = accel.code_tcm.instruction
            accel.cpu.dcache_port = accel.bus.cpu_side_ports
        accel.cpu.mmu.connectWalkerPorts(
            accel.bus.cpu_side_ports, accel.bus.cpu_side_ports
        )
        accel.system_port = accel.bus.cpu_side_ports
        accel.control = RTAccelControl(
            cpu=accel.cpu,
            registers=AddrRange(0x10000000, size=0x1000),
            data_tcm_range=accel.mem_ranges[1],
            host_range=system.mem_ranges[0],
            max_outstanding=args.dma_outstanding,
            coherent_dma=cached,
        )
        accel.control.pio = accel.bus.mem_side_ports
        accel.control.dma = accel.bus.cpu_side_ports
        accel.control.dma_write = accel.bus.cpu_side_ports
        if cached:
            accel.control.dma_clean = accel.bus.cpu_side_ports
        accel.wrapper = RTGem5Accelerator(
            host=system.adapters[i],
            index=i,
            queue_depth=args.queue_depth,
            trace_file=(str(outdir / f"accel{i}.trace") if args.trace else ""),
            request_delay=args.transport_delay,
            response_delay=args.transport_delay,
            system_ranges=[system.mem_ranges[0], system.msi.registers],
        )
        accel.wrapper.mmio = accel.bus.cpu_side_ports
        accel.wrapper.memory = accel.bus.mem_side_ports
    for adapter in system.adapters:
        adapter.pio = system.membus.mem_side_ports
        adapter.dma = system.membus.cpu_side_ports
    root = Root(full_system=True, system=system)
    # Keep modeled link stages identical in serial and parallel configurations.
    m5.ticks.fixGlobalFrequency()
    # Both directions have this delay, the largest safe PDES quantum.
    root.sim_quantum = m5.ticks.fromSeconds(
        m5.util.convert.toLatency(args.transport_delay)
    )
    # Firmware waits past the deferred global statistics event before
    # submitting jobs or exporting their output.
    system.init_param = (int(root.sim_quantum) + 999) // 1000
    return root, system
