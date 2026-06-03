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

"""TrafficGen -> SystemXBar/NoCache -> DDR4 memory testbench.

This script drives synthetic TrafficGen traffic directly into one of three
memory backends configured for the same DDR4-2400 4Gb x8 single-channel,
single-rank, 4GiB device-under-test:

- ``gem5``     : gem5 native ``MemCtrl`` + a custom ``DDR4_2400_8x8_4GiB``
                 ``DRAMInterface`` whose timing is taken from the DRAMSys
                 reference memspec ``JEDEC_4Gb_DDR4-2400_8bit_A.json``.
- ``dramsys``  : DRAMSys with the same JEDEC DDR4-2400 4Gb x8 memspec.
- ``dramsim3`` : DRAMSim3 with the derived
                 ``DDR4_4Gb_x8_2400_1rank_4GiB.ini`` config.

All three backends expose a single 4GiB range, one channel, one rank, eight
x8 devices, and DDR4-2400 (tCK = 0.833 ns) timing. See
``DDR4-2400-4Gb-x8-single channel-single rank-4GiB-study.md`` for the full
apples-to-apples requirements.
"""

import argparse
import json
from pathlib import Path

import m5
from m5.util.convert import toMemorySize

from gem5.components.boards.test_board import TestBoard
from gem5.components.cachehierarchies.classic.no_cache import NoCache
from gem5.components.memory.dram_interfaces.ddr4 import DDR4_2400_8x8
from gem5.components.memory.dramsim_3 import SingleChannel as DRAMSim3Single
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

# DRAMSys reference memspec for this study (source of truth for timing):
#   ext/dramsys/DRAMSys/configs/memspec/JEDEC_4Gb_DDR4-2400_8bit_A.json
DRAMSYS_DDR4_2400_CONFIG = (
    "ext/dramsys/gem5_configs/ddr4-2400-4gb-x8-gem5-se.json"
)

# DRAMSim3 derived config (channel_size=4096 -> one rank, CWL=16, tRTP=12).
# Tracked in the gem5 tree so the comparison does not depend on a file inside
# the external DRAMSim3 clone; loaded directly by path.
DRAMSIM3_DDR4_2400_CONFIG = "ext/dramsim3/DDR4_4Gb_x8_2400_1rank_4GiB.ini"


class DDR4_2400_8x8_4GiB(DDR4_2400_8x8):
    """A single DDR4-2400 4Gb x8 channel: 1 rank, 8 devices, 4GiB.

    Geometry and timing are taken from the DRAMSys reference memspec
    ``JEDEC_4Gb_DDR4-2400_8bit_A.json`` (the study's source of truth) with
    cycle counts converted to nanoseconds at tCK = 0.833 ns.

    Notes on parameters that have no direct DRAMSys/JEDEC equivalent:

    - ``tRTW`` (read-to-write bus turnaround) is a gem5-internal knob with no
      single JEDEC cycle value; the DDR4-2400 base value (2 tCK = 1.666 ns)
      is retained.
    - ``tCS`` is the rank-to-rank switching delay; it is set from RTRS = 1
      (0.833 ns) but does not affect this single-rank device.
    """

    # --- Geometry: DDR4-2400 4Gb x8, single rank, 4GiB ---
    device_size = "512MiB"  # 4Gbit per device
    device_bus_width = 8  # x8
    devices_per_rank = 8  # 8 devices -> 64-bit channel
    ranks_per_channel = 1  # single rank
    burst_length = 8  # BL8 -> 64-byte burst
    bank_groups_per_rank = 4
    banks_per_rank = 16
    device_rowbuffer_size = "1KiB"  # 1024 columns x8 = 1KiB/device

    # --- Timing converted from the DRAMSys reference at tCK = 0.833 ns ---
    tCK = "0.833ns"
    tBURST = "3.332ns"  # CCD_S = 4 (BL8 over x64)
    tCCD_L = "4.998ns"  # CCD_L = 6

    tCL = "13.328ns"  # CL / RL = 16
    tRCD = "13.328ns"  # RCD = 16
    tRP = "13.328ns"  # RP = 16
    tRAS = "32.487ns"  # RAS = 39

    tRRD = "3.332ns"  # RRD_S = 4
    tRRD_L = "4.998ns"  # RRD_L = 6
    tXAW = "21.658ns"  # FAW = 26
    activation_limit = 4

    tRFC = "259.896ns"  # RFC1 = 312
    tREFI = "7796.88ns"  # REFI = 9360

    tWR = "14.994ns"  # WR = 18
    tWTR = "2.499ns"  # WTR_S = 3
    tWTR_L = "7.497ns"  # WTR_L = 9
    tRTP = "9.996ns"  # RTP = 12

    tXP = "6.664ns"  # XP = 8
    tXS = "269.892ns"  # XS = 324

    tRTW = "1.666ns"  # gem5-internal; no JEDEC equivalent
    tCS = "0.833ns"  # RTRS = 1 (single rank: unused)


DDR4_INTERFACES = {
    "2400-x8-4gib": DDR4_2400_8x8_4GiB,
}


def _memory_size(value: str) -> int:
    return int(toMemorySize(value))


def _parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run synthetic TrafficGen requests through NoCache into "
        "a gem5, DRAMSys, or DRAMSim3 DDR4-2400 4Gb x8 backend."
    )

    parser.add_argument(
        "--memory-backend",
        choices=["gem5", "dramsys", "dramsim3"],
        default="gem5",
        help="Memory backend attached behind the SystemXBar.",
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
        default="4GiB",
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
        "--gem5-ddr4-interface",
        choices=sorted(DDR4_INTERFACES),
        default="2400-x8-4gib",
        help="gem5 DDR4 interface used by the gem5 memory backend.",
    )
    parser.add_argument(
        "--dram-addr-mapping",
        default="RoCoRaBaCh",
        help="gem5 DRAMInterface address mapping (default RoCoRaBaCh, which "
        "interleaves banks at cache-line granularity to match DRAMSys and "
        "DRAMSim3).",
    )
    parser.add_argument(
        "--gem5-page-policy",
        default="open",
        help="gem5 DRAMInterface page management policy.",
    )
    parser.add_argument(
        "--gem5-sched-policy",
        default="frfcfs",
        help="gem5 MemCtrl scheduling policy.",
    )
    parser.add_argument(
        "--gem5-read-buffer-size",
        type=int,
        default=32,
        help="gem5 MemCtrl read queue entries.",
    )
    parser.add_argument(
        "--gem5-write-buffer-size",
        type=int,
        default=32,
        help="gem5 MemCtrl write queue entries.",
    )
    parser.add_argument(
        "--dramsys-config",
        default=DRAMSYS_DDR4_2400_CONFIG,
        help="DRAMSys base JSON configuration.",
    )
    parser.add_argument(
        "--dramsys-resource-dir",
        default=None,
        help="Optional DRAMSys resource directory.",
    )
    parser.add_argument(
        "--dramsim3-config",
        default=DRAMSIM3_DDR4_2400_CONFIG,
        help="DRAMSim3 config name under ext/dramsim3/DRAMsim3/configs "
        "(without the .ini suffix).",
    )
    parser.add_argument(
        "--sys-clock",
        default="1.2GHz",
        help="Clock for the board and SystemXBar (controller/requestor "
        "domain). Default 1.2GHz matches the DDR4-2400 command clock.",
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
        memory = ChanneledMemory(
            DDR4_INTERFACES[args.gem5_ddr4_interface],
            num_channels=1,
            interleaving_size=args.cache_line_size,
            size=args.mem_size,
            addr_mapping=args.dram_addr_mapping,
        )
        # Apply FR-FCFS open-page controller policy on every controller and
        # the matching queue limits requested by the study.
        for ctrl in memory.get_memory_controllers():
            ctrl.mem_sched_policy = args.gem5_sched_policy
        for dram in memory.get_mem_interfaces():
            dram.page_policy = args.gem5_page_policy
            dram.read_buffer_size = args.gem5_read_buffer_size
            dram.write_buffer_size = args.gem5_write_buffer_size
        return memory

    if args.memory_backend == "dramsim3":
        return DRAMSim3Single(args.dramsim3_config, args.mem_size)

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
        "gem5_binary": str(getattr(m5.options, "binary", "")),
        "config_script": __file__,
        "backend": args.memory_backend,
        "topology": "TrafficGen -> SystemXBar/NoCache -> memory",
        "traffic_pattern": args.traffic_pattern,
        "read_percent": read_percent,
        "rate": args.rate,
        "duration": args.duration,
        "warmup": "none",
        "data_limit_bytes": _memory_size(args.data_limit),
        "num_requestors": args.num_generators,
        "block_size": args.cache_line_size,
        "memory_config_file": {
            "gem5": f"DDR4_2400_8x8_4GiB ({args.dram_addr_mapping})",
            "dramsys": args.dramsys_config,
            "dramsim3": args.dramsim3_config,
        }[args.memory_backend],
        "memory_size": args.mem_size,
        "memory_size_bytes": _memory_size(args.mem_size),
        "memory_tCK": "0.833ns",
        "traffic_address_range": max_addr - args.min_addr,
        "min_addr": args.min_addr,
        "max_addr": max_addr,
        "random_seed": "TrafficGen default (deterministic)",
        "controller_clock": args.sys_clock,
        "requestor_clock": args.sys_clock,
        "system_clock": args.sys_clock,
        "gem5_ddr4_interface": args.gem5_ddr4_interface,
        "gem5_page_policy": args.gem5_page_policy,
        "gem5_sched_policy": args.gem5_sched_policy,
        "gem5_read_buffer_size": args.gem5_read_buffer_size,
        "gem5_write_buffer_size": args.gem5_write_buffer_size,
        "dram_addr_mapping": args.dram_addr_mapping,
        "dramsys_config": args.dramsys_config,
        "dramsim3_config": args.dramsim3_config,
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
        cache_hierarchy=NoCache(),
    )

    simulator = Simulator(board=board)
    simulator.run()
    _write_metadata(args, simulator)


if __name__ == "__m5_main__":
    main()
