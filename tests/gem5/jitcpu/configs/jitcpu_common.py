# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause

"""Ruby configuration and stat helpers shared by the JitCPU test configs.

The bare-metal and Linux configurations validate the same invariant after
every switch: a JitCPU phase must leave the timing hierarchy completely
untouched, and an O3 phase must exercise every RNF and memory controller.
Both therefore need the same view of network, cache and memory activity.
"""

import argparse

from common import Options
from jitcpu_16core_mesh import configure_ruby_options
from ruby import Ruby

from m5.objects import Root
from m5.stats.gem5stats import get_simstat


def ruby_options(network, num_cpus, num_dirs, num_l3caches, mesh_4x4):
    """Build the Ruby option namespace the CHI scripts expect."""
    parser = argparse.ArgumentParser(add_help=False)
    Options.addCommonOptions(parser)
    Ruby.define_options(parser)
    options = parser.parse_args([])
    options.num_cpus = num_cpus
    options.num_dirs = num_dirs
    options.num_l3caches = num_l3caches
    # Several DRAM controllers expose interleaved backing ranges, which QEMU
    # cannot map as one host-contiguous region. Ruby's canonical functional
    # backing store preserves CHI timing while giving JitCPU a direct map.
    options.access_backing_store = num_dirs > 1
    options.topology = "Crossbar"
    options.network = network
    configure_ruby_options(options, mesh_4x4)
    return options


def stat_value(name):
    value = Root.getInstance().resolveStat(name).value
    if isinstance(value, list):
        return int(sum(value))
    return int(value)


def cpu_stat(cpu, leaf, o3_cpus):
    """Read a per-CPU commit stat, whichever CPU model owns the core."""
    try:
        return stat_value(f"{cpu.path()}.{leaf}")
    except KeyError:
        # SimObject vector paths are zero-padded at 10+ entries, while the
        # corresponding stat names are not. Use the stable CPU id spelling.
        collection = (
            "o3" if any(cpu is candidate for candidate in o3_cpus) else "cpu"
        )
        return stat_value(f"system.{collection}{int(cpu.cpu_id)}.{leaf}")


def ruby_router_messages(ruby_system, network):
    """Messages carried by the timing interconnect, or None if unavailable.

    SimpleNetwork accounts every switch traversal in per-throttle group
    stats. Garnet still registers its packet counters through the legacy
    stat API, which pystats does not expose, so there is nothing to read
    there. Callers must therefore treat None as "unknown" and fall back to
    the controller-side checks below, which work on any network.
    """
    if network != "simple":
        return None

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


def ruby_cache_accesses(jit_cpus):
    """Demand accesses seen by each requester's CHI cache controllers."""
    accesses = []
    for cpu in jit_cpus:
        cpu_accesses = 0
        for controller in (cpu.l1d, cpu.l1i, cpu.l2):
            cache_stats = get_simstat(controller)["cache"]
            cpu_accesses += int(cache_stats["m_demand_hits"].value)
            cpu_accesses += int(cache_stats["m_demand_misses"].value)
        accesses.append(cpu_accesses)
    return accesses


def ruby_memory_bytes(mem_ctrls):
    return [
        stat_value(f"{controller.path()}.bytesReadSys")
        + stat_value(f"{controller.path()}.bytesWrittenSys")
        for controller in mem_ctrls
    ]
