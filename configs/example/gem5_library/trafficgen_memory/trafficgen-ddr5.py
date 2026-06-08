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

"""TrafficGen -> SystemXBar/NoCache -> DDR5 memory testbench.

The native gem5 and Ramulator2 backends model the same DDR5-4800AN-style
device-under-test: two independent 32-bit channels, one rank per channel,
four x8 devices per rank, 16Gb devices, 8 bank groups, and 4 banks per
bank group.
"""

import argparse
import json
import sys
from pathlib import Path
from typing import List, Sequence, Tuple

import _m5.enum_AddrMap
import m5
from m5.objects import AddrMap, PyTrafficGen, Ramulator2
from m5.params import AddrRange, Port
from m5.ticks import fromSeconds
from m5.util.convert import toMemorySize
from m5.util.convert import toLatency, toMemoryBandwidth

from gem5.components.boards.abstract_board import AbstractBoard
from gem5.components.boards.test_board import TestBoard
from gem5.components.cachehierarchies.classic.no_cache import NoCache
from gem5.components.memory.abstract_memory_system import AbstractMemorySystem
from gem5.components.memory.dram_interfaces.ddr5 import DDR5_4400_4x8
from gem5.components.memory.memory import ChanneledMemory
from gem5.components.processors.abstract_generator import AbstractGenerator
from gem5.components.processors.abstract_generator import partition_range
from gem5.components.processors.linear_generator import LinearGenerator
from gem5.components.processors.linear_generator_core import (
    LinearGeneratorCore,
)
from gem5.components.processors.random_generator import RandomGenerator
from gem5.components.processors.random_generator_core import (
    RandomGeneratorCore,
)
from gem5.simulate.simulator import Simulator

TRAFFIC_PATTERNS = {
    "linear-read": ("standard", "linear", 100),
    "linear-write": ("standard", "linear", 0),
    "linear-mixed": ("standard", "linear", None),
    "random-read": ("standard", "random", 100),
    "random-mixed": ("standard", "random", None),
    "probe-stream-read": ("probe-stream", "linear", 100),
    "probe-stream-mixed": ("probe-stream", "linear", None),
}


class DDR5_4800_4x8_16GiB(DDR5_4400_4x8):
    """One 32-bit DDR5-4800 channel using 16Gb x8 devices."""

    device_size = "2GiB"
    device_bus_width = 8
    devices_per_rank = 4
    ranks_per_channel = 1
    burst_length = 16
    device_rowbuffer_size = "1KiB"
    bank_groups_per_rank = 8
    banks_per_rank = 32

    tCK = "0.416ns"
    tBURST = "3.328ns"
    tCCD_L = "4.992ns"

    tCL = "14.144ns"
    tCWL = "13.312ns"
    tRCD = "14.144ns"
    tRP = "14.144ns"
    tRAS = "32.032ns"

    tRRD = "3.328ns"
    tRRD_L = "4.992ns"
    tXAW = "20.384ns"
    activation_limit = 4

    tRFC = "295ns"
    tREFI = "3.9us"

    tPPD = "0.832ns"
    tWR = "29.952ns"
    tRTP = "7.488ns"
    tRTW = "1.872ns"
    tWTR = "15.812ns"
    tWTR_L = "23.312ns"

    tXP = "7.5ns"
    tXS = "295ns"
    tCS = "0.832ns"


class Ramulator2DDR5Memory(AbstractMemorySystem):
    def __init__(self, config: str, size: str) -> None:
        super().__init__()
        self._size = int(toMemorySize(size))
        self.ramulator = Ramulator2(ramulator_config=config)

    def incorporate_memory(self, board: AbstractBoard) -> None:
        pass

    def get_mem_ports(self) -> Sequence[Tuple[AddrRange, Port]]:
        return [(self.ramulator.range, self.ramulator.port)]

    def get_memory_controllers(self):
        return [self.ramulator]

    def get_mem_interfaces(self):
        return [self.ramulator]

    def get_size(self) -> int:
        return self._size

    def set_memory_range(self, ranges: List[AddrRange]) -> None:
        if len(ranges) != 1 or ranges[0].size() != self._size:
            raise Exception(
                "Ramulator2 DDR5 requires one range matching "
                f"{self._size} bytes"
            )
        self.ramulator.range = ranges[0]

    def get_uninterleaved_range(self) -> List[AddrRange]:
        return [self.ramulator.range]


class DramGeneratorCore(LinearGeneratorCore):
    def __init__(
        self,
        duration: str,
        rate: str,
        block_size: int,
        min_addr: int,
        max_addr: int,
        rd_perc: int,
        data_limit: int,
        num_seq_pkts: int,
        page_size: int,
        banks: int,
        banks_util: int,
        addr_mapping: str,
        ranks: int,
    ) -> None:
        super().__init__(
            duration=duration,
            rate=rate,
            block_size=block_size,
            min_addr=min_addr,
            max_addr=max_addr,
            rd_perc=rd_perc,
            data_limit=data_limit,
        )
        self.generator = PyTrafficGen()
        self._num_seq_pkts = num_seq_pkts
        self._page_size = page_size
        self._banks = banks
        self._banks_util = banks_util
        self._addr_mapping = addr_mapping
        self._ranks = ranks

    def _create_traffic(self):
        duration = fromSeconds(toLatency(self._duration))
        rate = toMemoryBandwidth(self._rate)
        period = fromSeconds(self._block_size / rate)
        addr_map = getattr(_m5.enum_AddrMap.enum_AddrMap, self._addr_mapping)
        yield self.generator.createDram(
            duration,
            self._min_addr,
            self._max_addr,
            self._block_size,
            period,
            period,
            self._rd_perc,
            self._data_limit,
            self._num_seq_pkts,
            self._page_size,
            self._banks,
            self._banks_util,
            addr_map,
            self._ranks,
        )
        yield self.generator.createExit(0)


class ProbeStreamGenerator(AbstractGenerator):
    def __init__(
        self,
        duration: str,
        stream_rate: str,
        probe_rate: str,
        num_stream_cores: int,
        block_size: int,
        min_addr: int,
        stream_max_addr: int,
        probe_max_addr: int,
        stream_read_percent: int,
        stream_data_limit: int,
        probe_data_limit: int,
        stream_generator: str,
        stream_num_seq_pkts: int,
        stream_page_size: int,
        stream_banks: int,
        stream_banks_util: int,
        dram_addr_mapping: str,
    ) -> None:
        stream_ranges = partition_range(
            min_addr, stream_max_addr, num_stream_cores
        )
        if stream_generator == "dram":
            stream_cores = [
                DramGeneratorCore(
                    duration=duration,
                    rate=stream_rate,
                    block_size=block_size,
                    min_addr=stream_ranges[index][0],
                    max_addr=stream_ranges[index][1],
                    rd_perc=stream_read_percent,
                    data_limit=stream_data_limit,
                    num_seq_pkts=stream_num_seq_pkts,
                    page_size=stream_page_size,
                    banks=stream_banks,
                    banks_util=stream_banks_util,
                    addr_mapping=dram_addr_mapping,
                    ranks=1,
                )
                for index in range(num_stream_cores)
            ]
        else:
            stream_cores = [
                LinearGeneratorCore(
                    duration=duration,
                    rate=stream_rate,
                    block_size=block_size,
                    min_addr=stream_ranges[index][0],
                    max_addr=stream_ranges[index][1],
                    rd_perc=stream_read_percent,
                    data_limit=stream_data_limit,
                )
                for index in range(num_stream_cores)
            ]
        probe_core = RandomGeneratorCore(
            duration=duration,
            rate=probe_rate,
            block_size=block_size,
            min_addr=min_addr,
            max_addr=probe_max_addr,
            rd_perc=100,
            data_limit=probe_data_limit,
        )
        probe_core.generator.max_outstanding_reqs = 1
        super().__init__(cores=[*stream_cores, probe_core])

    def start_traffic(self) -> None:
        for core in self.cores:
            core.start_traffic()


def _memory_size(value: str) -> int:
    return int(toMemorySize(value))


def _parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run TrafficGen requests through NoCache into a native "
        "gem5 or Ramulator2 DDR5-4800 backend."
    )
    parser.add_argument(
        "--memory-backend",
        choices=["gem5", "ramulator"],
        default="gem5",
    )
    parser.add_argument(
        "--traffic-pattern",
        choices=sorted(TRAFFIC_PATTERNS),
        default="linear-read",
    )
    parser.add_argument("--rate", default="1GiB/s")
    parser.add_argument("--probe-rate", default="1GiB/s")
    parser.add_argument("--duration", default="30us")
    parser.add_argument("--data-limit", default="0B")
    parser.add_argument("--probe-data-limit", default="0B")
    parser.add_argument("--num-generators", type=int, default=1)
    parser.add_argument("--cache-line-size", type=int, default=64)
    parser.add_argument("--mem-size", default="16GiB")
    parser.add_argument("--addr-range", default="1GiB")
    parser.add_argument("--probe-addr-range", default="1GiB")
    parser.add_argument(
        "--min-addr",
        type=lambda value: int(value, 0),
        default=0,
    )
    parser.add_argument("--mixed-read-percent", type=int, default=50)
    parser.add_argument("--dram-addr-mapping", default="RoCoRaBaCh")
    parser.add_argument("--gem5-page-policy", default="open")
    parser.add_argument("--gem5-sched-policy", default="frfcfs")
    parser.add_argument("--gem5-read-buffer-size", type=int, default=32)
    parser.add_argument("--gem5-write-buffer-size", type=int, default=32)
    parser.add_argument("--ramulator-read-buffer-size", type=int, default=32)
    parser.add_argument("--ramulator-write-buffer-size", type=int, default=32)
    parser.add_argument(
        "--curve-stream-generator",
        choices=["dram", "linear"],
        default="dram",
    )
    parser.add_argument("--stream-num-seq-pkts", type=int, default=8)
    parser.add_argument("--stream-page-size", default="4KiB")
    parser.add_argument("--stream-banks", type=int, default=32)
    parser.add_argument("--stream-banks-util", type=int, default=32)
    parser.add_argument(
        "--ramulator-python-path",
        default="ext/ramulator2/ramulator2/python",
    )
    parser.add_argument("--sys-clock", default="2.4GHz")
    parser.add_argument("--metadata-file", default="run_metadata.json")
    return parser.parse_args()


def _validate_arguments(args: argparse.Namespace) -> None:
    if args.num_generators < 1:
        raise ValueError("--num-generators must be at least 1")
    if args.cache_line_size != 64:
        raise ValueError("DDR5-4800 comparison expects 64-byte requests")
    if args.mixed_read_percent < 0 or args.mixed_read_percent > 100:
        raise ValueError("--mixed-read-percent must be in [0, 100]")
    if args.dram_addr_mapping not in AddrMap.map:
        raise ValueError(
            f"Unknown DRAM address mapping {args.dram_addr_mapping}"
        )
    if (
        args.stream_banks_util < 1
        or args.stream_banks_util > args.stream_banks
    ):
        raise ValueError("--stream-banks-util must be in [1, --stream-banks]")
    if args.stream_num_seq_pkts < 1:
        raise ValueError("--stream-num-seq-pkts must be at least 1")

    mem_size = _memory_size(args.mem_size)
    for range_arg in (args.addr_range, args.probe_addr_range):
        if args.min_addr + _memory_size(range_arg) > mem_size:
            raise ValueError("Traffic address range must fit in --mem-size")


def _ramulator_config(args: argparse.Namespace) -> str:
    sys.path.insert(0, args.ramulator_python_path)
    import ramulator

    controllers = []
    for _ in range(2):
        dram = ramulator.dram.DDR5(
            org_preset="DDR5_16Gb_x8",
            timing_preset="DDR5_4800AN",
            rank=1,
        )
        controllers.append(
            ramulator.controller.GenericDDR(
                dram=dram,
                scheduler=ramulator.scheduler.FRFCFS(),
                refresh_manager=ramulator.refresh_manager.AllBank(),
                row_policy=ramulator.row_policy.Open(),
                addr_mapper=ramulator.addr_mapper.RoBaRaCoCh(),
                read_buffer_size=args.ramulator_read_buffer_size,
                write_buffer_size=args.ramulator_write_buffer_size,
            )
        )

    memory_system = ramulator.memory_system.GenericDRAM(
        clock_ratio=1,
        controllers=controllers,
        channel_mapper=ramulator.channel_mapper.CacheLineInterleave(),
    )
    config = {
        "frontend": {"impl": "External", "clock_ratio": 1},
        "memory_system": memory_system.to_config(),
    }
    return json.dumps(config)


def _make_memory(args: argparse.Namespace):
    if args.memory_backend == "gem5":
        memory = ChanneledMemory(
            DDR5_4800_4x8_16GiB,
            num_channels=2,
            interleaving_size=args.cache_line_size,
            size=args.mem_size,
            addr_mapping=args.dram_addr_mapping,
        )
        for ctrl in memory.get_memory_controllers():
            ctrl.mem_sched_policy = args.gem5_sched_policy
        for dram in memory.get_mem_interfaces():
            dram.page_policy = args.gem5_page_policy
            dram.read_buffer_size = args.gem5_read_buffer_size
            dram.write_buffer_size = args.gem5_write_buffer_size
        return memory

    return Ramulator2DDR5Memory(_ramulator_config(args), args.mem_size)


def _traffic_parameters(args: argparse.Namespace):
    mode, generator_kind, read_percent = TRAFFIC_PATTERNS[args.traffic_pattern]
    if read_percent is None:
        read_percent = args.mixed_read_percent
    return mode, generator_kind, read_percent


def _make_generator(args: argparse.Namespace):
    mode, generator_kind, read_percent = _traffic_parameters(args)
    max_addr = args.min_addr + _memory_size(args.addr_range)
    probe_max_addr = args.min_addr + _memory_size(args.probe_addr_range)

    if mode == "probe-stream":
        return ProbeStreamGenerator(
            duration=args.duration,
            stream_rate=args.rate,
            probe_rate=args.probe_rate,
            num_stream_cores=args.num_generators,
            block_size=args.cache_line_size,
            min_addr=args.min_addr,
            stream_max_addr=max_addr,
            probe_max_addr=probe_max_addr,
            stream_read_percent=read_percent,
            stream_data_limit=_memory_size(args.data_limit),
            probe_data_limit=_memory_size(args.probe_data_limit),
            stream_generator=args.curve_stream_generator,
            stream_num_seq_pkts=args.stream_num_seq_pkts,
            stream_page_size=_memory_size(args.stream_page_size),
            stream_banks=args.stream_banks,
            stream_banks_util=args.stream_banks_util,
            dram_addr_mapping=args.dram_addr_mapping,
        )

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
    mode, _, read_percent = _traffic_parameters(args)
    metadata = {
        "gem5_binary": str(getattr(m5.options, "binary", "")),
        "config_script": __file__,
        "backend": args.memory_backend,
        "topology": "TrafficGen -> SystemXBar/NoCache -> memory",
        "traffic_pattern": args.traffic_pattern,
        "traffic_mode": mode,
        "read_percent": read_percent,
        "rate": args.rate,
        "probe_rate": args.probe_rate,
        "duration": args.duration,
        "data_limit_bytes": _memory_size(args.data_limit),
        "probe_data_limit_bytes": _memory_size(args.probe_data_limit),
        "num_requestors": (
            args.num_generators + 1
            if mode == "probe-stream"
            else args.num_generators
        ),
        "num_stream_generators": (
            args.num_generators if mode == "probe-stream" else 0
        ),
        "block_size": args.cache_line_size,
        "memory_config": {
            "gem5": f"DDR5_4800_4x8_16GiB ({args.dram_addr_mapping})",
            "ramulator": "DDR5_16Gb_x8 DDR5_4800AN, 2 channels",
        }[args.memory_backend],
        "memory_size": args.mem_size,
        "memory_size_bytes": _memory_size(args.mem_size),
        "memory_tCK": "0.416ns",
        "traffic_address_range": _memory_size(args.addr_range),
        "probe_address_range": _memory_size(args.probe_addr_range),
        "min_addr": args.min_addr,
        "system_clock": args.sys_clock,
        "gem5_page_policy": args.gem5_page_policy,
        "gem5_sched_policy": args.gem5_sched_policy,
        "gem5_read_buffer_size": args.gem5_read_buffer_size,
        "gem5_write_buffer_size": args.gem5_write_buffer_size,
        "ramulator_read_buffer_size": args.ramulator_read_buffer_size,
        "ramulator_write_buffer_size": args.ramulator_write_buffer_size,
        "dram_addr_mapping": args.dram_addr_mapping,
        "curve_stream_generator": args.curve_stream_generator,
        "stream_num_seq_pkts": args.stream_num_seq_pkts,
        "stream_page_size": _memory_size(args.stream_page_size),
        "stream_banks": args.stream_banks,
        "stream_banks_util": args.stream_banks_util,
        "final_tick": m5.curTick(),
        "exit_cause": simulator.get_last_exit_event_cause(),
        "exit_code": simulator.get_last_exit_event_code(),
    }

    output_path = Path(m5.options.outdir) / args.metadata_file
    output_path.write_text(json.dumps(metadata, indent=2) + "\n")


def main() -> None:
    args = _parse_arguments()
    _validate_arguments(args)

    board = TestBoard(
        clk_freq=args.sys_clock,
        generator=_make_generator(args),
        memory=_make_memory(args),
        cache_hierarchy=NoCache(),
    )

    simulator = Simulator(board=board)
    simulator.run()
    _write_metadata(args, simulator)


if __name__ == "__m5_main__":
    main()
