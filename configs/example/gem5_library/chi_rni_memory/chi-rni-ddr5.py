# Copyright (c) 2026 The Regents of the University of California
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

"""TrafficGen -> CHI RN-I -> HNF -> SNF -> memory testbench.

This script is intended to be launched from the gem5 repository root. The
gem5 memory backend uses a single DDR5 channel. The DRAMSys backend uses the
JSON configuration passed with --dramsys-config; DRAMSys v5.3.1, the version
currently verified by ext/dramsys/README, does not ship DDR5 DRAMSys models.
"""

import argparse
import json
from pathlib import Path

import m5
from m5.util.convert import toMemorySize

from gem5.components.boards.test_board import TestBoard
from gem5.components.cachehierarchies.chi.rni_cache_hierarchy import (
    RNICacheHierarchy,
)
from gem5.components.memory.dram_interfaces.ddr5 import (
    DDR5_4400_4x8,
    DDR5_6400_4x8,
    DDR5_8400_4x8,
)
from gem5.components.memory.dramsys import DRAMSysMem
from gem5.components.memory.memory import ChanneledMemory
from gem5.components.processors.linear_generator import LinearGenerator
from gem5.components.processors.random_generator import RandomGenerator
from gem5.simulate.simulator import Simulator

TRAFFIC_PATTERNS = {
    "linear-read": ("linear", 100),
    "linear-write": ("linear", 0),
    "linear-mixed": ("linear", None),
    "random-read": ("random", 100),
    "random-mixed": ("random", None),
}

DDR5_INTERFACES = {
    "4400": DDR5_4400_4x8,
    "6400": DDR5_6400_4x8,
    "8400": DDR5_8400_4x8,
}


def _memory_size(value: str) -> int:
    return int(toMemorySize(value))


def _parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run synthetic traffic through a cacheless CHI RN-I "
        "hierarchy into either gem5 DDR5 memory or DRAMSys."
    )

    parser.add_argument(
        "--memory-backend",
        choices=["gem5", "dramsys"],
        default="gem5",
        help="Memory backend attached behind the CHI SNF.",
    )
    parser.add_argument(
        "--traffic-pattern",
        choices=sorted(TRAFFIC_PATTERNS),
        default="linear-read",
        help="Traffic pattern emitted by TrafficGen.",
    )
    parser.add_argument(
        "--rate",
        default="1GiB/s",
        help="Offered aggregate traffic bandwidth.",
    )
    parser.add_argument(
        "--duration",
        default="20us",
        help="Traffic generation duration.",
    )
    parser.add_argument(
        "--data-limit",
        default="0B",
        help="Optional per-generator data limit. 0B disables the limit.",
    )
    parser.add_argument(
        "--num-generators",
        type=int,
        default=1,
        help="Number of independent TrafficGen requestors.",
    )
    parser.add_argument(
        "--cache-line-size",
        type=int,
        default=64,
        help="Traffic block size and memory interleaving granularity.",
    )
    parser.add_argument(
        "--mem-size",
        default="2GiB",
        help="Address range exposed by the selected memory backend.",
    )
    parser.add_argument(
        "--addr-range",
        default=None,
        help="Traffic address range. Defaults to the full memory size.",
    )
    parser.add_argument(
        "--min-addr",
        type=lambda value: int(value, 0),
        default=0,
        help="Lowest generated address.",
    )
    parser.add_argument(
        "--mixed-read-percent",
        type=int,
        default=50,
        help="Read percentage for mixed traffic patterns.",
    )
    parser.add_argument(
        "--ddr5-data-rate",
        choices=sorted(DDR5_INTERFACES),
        default="4400",
        help="gem5 DDR5 interface data rate.",
    )
    parser.add_argument(
        "--dram-addr-mapping",
        default=None,
        help="Optional gem5 DRAMInterface address mapping override.",
    )
    parser.add_argument(
        "--dramsys-config",
        default="ext/dramsys/gem5_configs/ddr4-gem5-se.json",
        help="DRAMSys base JSON configuration.",
    )
    parser.add_argument(
        "--dramsys-resource-dir",
        default=None,
        help="Optional DRAMSys resource directory.",
    )
    parser.add_argument(
        "--sys-clock",
        default="3GHz",
        help="Clock for the board, Ruby network, HNF, and SNF controllers.",
    )
    parser.add_argument(
        "--metadata-file",
        default="run_metadata.json",
        help="Metadata JSON emitted under the gem5 output directory.",
    )

    return parser.parse_args()


def _validate_arguments(args: argparse.Namespace) -> None:
    if args.num_generators < 1:
        raise ValueError("--num-generators must be at least 1")
    if args.cache_line_size <= 0:
        raise ValueError("--cache-line-size must be positive")
    if args.mixed_read_percent < 0 or args.mixed_read_percent > 100:
        raise ValueError("--mixed-read-percent must be in [0, 100]")

    mem_size = _memory_size(args.mem_size)
    addr_range = (
        mem_size if args.addr_range is None else _memory_size(args.addr_range)
    )
    if addr_range <= 0:
        raise ValueError("--addr-range must be positive")
    if args.min_addr < 0:
        raise ValueError("--min-addr must be non-negative")
    if args.min_addr + addr_range > mem_size:
        raise ValueError("Traffic range must fit inside --mem-size")

    data_limit = _memory_size(args.data_limit)
    if data_limit < 0:
        raise ValueError("--data-limit must be non-negative")

    if args.memory_backend == "dramsys":
        dramsys_config = Path(args.dramsys_config)
        if not dramsys_config.exists():
            raise FileNotFoundError(
                f"DRAMSys configuration '{dramsys_config}' does not exist"
            )


def _make_memory(args: argparse.Namespace):
    if args.memory_backend == "gem5":
        return ChanneledMemory(
            DDR5_INTERFACES[args.ddr5_data_rate],
            num_channels=1,
            interleaving_size=args.cache_line_size,
            size=args.mem_size,
            addr_mapping=args.dram_addr_mapping,
        )

    return DRAMSysMem(
        configuration=args.dramsys_config,
        size=args.mem_size,
        resource_directory=args.dramsys_resource_dir,
    )


def _traffic_parameters(args: argparse.Namespace):
    generator_kind, read_percent = TRAFFIC_PATTERNS[args.traffic_pattern]
    if read_percent is None:
        read_percent = args.mixed_read_percent

    mem_size = _memory_size(args.mem_size)
    addr_range = (
        mem_size if args.addr_range is None else _memory_size(args.addr_range)
    )
    max_addr = args.min_addr + addr_range

    return generator_kind, read_percent, max_addr


def _make_generator(args: argparse.Namespace):
    generator_kind, read_percent, max_addr = _traffic_parameters(args)
    generator_class = (
        LinearGenerator if generator_kind == "linear" else RandomGenerator
    )

    return generator_class(
        num_cores=args.num_generators,
        duration=args.duration,
        rate=args.rate,
        block_size=args.cache_line_size,
        min_addr=args.min_addr,
        max_addr=max_addr,
        rd_perc=read_percent,
        data_limit=_memory_size(args.data_limit),
    )


def _write_metadata(args: argparse.Namespace, simulator: Simulator) -> None:
    _, read_percent, max_addr = _traffic_parameters(args)
    metadata = {
        "memory_backend": args.memory_backend,
        "traffic_pattern": args.traffic_pattern,
        "rate": args.rate,
        "duration": args.duration,
        "data_limit_bytes": _memory_size(args.data_limit),
        "num_generators": args.num_generators,
        "cache_line_size": args.cache_line_size,
        "mem_size": args.mem_size,
        "mem_size_bytes": _memory_size(args.mem_size),
        "min_addr": args.min_addr,
        "max_addr": max_addr,
        "read_percent": read_percent,
        "sys_clock": args.sys_clock,
        "ddr5_data_rate": args.ddr5_data_rate,
        "dram_addr_mapping": args.dram_addr_mapping,
        "dramsys_config": args.dramsys_config,
        "dramsys_resource_dir": args.dramsys_resource_dir,
        "final_tick": m5.curTick(),
        "exit_cause": simulator.get_last_exit_event_cause(),
        "exit_code": simulator.get_last_exit_event_code(),
    }

    output_path = Path(m5.options.outdir) / args.metadata_file
    output_path.write_text(json.dumps(metadata, indent=2) + "\n")


def main() -> None:
    args = _parse_arguments()
    _validate_arguments(args)

    memory = _make_memory(args)
    generator = _make_generator(args)

    board = TestBoard(
        clk_freq=args.sys_clock,
        generator=generator,
        memory=memory,
        cache_hierarchy=RNICacheHierarchy(),
    )

    simulator = Simulator(board=board)
    simulator.run()
    _write_metadata(args, simulator)


if __name__ == "__m5_main__":
    main()
