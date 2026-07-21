# Copyright (c) 2026 The gem5 Authors
# SPDX-License-Identifier: BSD-3-Clause

"""Run two Verilated SCR1 cores with a selectable coherent memory system."""

import argparse
import json
from pathlib import Path

from gem5.components.boards.test_board import TestBoard
from gem5.components.cachehierarchies.classic import (
    private_l1_shared_l2_cache_hierarchy,
)
from gem5.components.memory.simple import SingleChannelSimpleMemory
from gem5.components.processors.rtl_core import RtlCore
from gem5.components.processors.rtl_processor import RtlProcessor
from gem5.simulate.simulator import Simulator
from m5.objects import AddrRange, RtlCosimTestController


SCR1_RESET_INPUTS = [
    "pwrup_rst_n",
    "rst_n",
    "cpu_rst_n",
    "test_rst_n",
    "trst_n",
]
SCR1_INTERRUPT_INPUTS = [f"irq_lines[{index}]" for index in range(16)] + [
    "soft_irq"
]
SCR1_IO_INPUTS = [
    "fuse_mhartid",
    "fuse_idcode",
    "test_mode",
    "rtc_clk",
    "tck",
    "tms",
    "tdi",
]


def _cache_hierarchy(memory_system: str):
    if memory_system == "classic":
        hierarchy = private_l1_shared_l2_cache_hierarchy
        return hierarchy.PrivateL1SharedL2CacheHierarchy(
            l1i_size="16KiB",
            l1i_assoc=4,
            l1d_size="16KiB",
            l1d_assoc=4,
            l2_size="256KiB",
            l2_assoc=8,
        )

    # Importing a Ruby hierarchy performs its protocol compatibility check.
    # Keep the import conditional so a classic-only binary remains usable.
    from gem5.components.cachehierarchies.ruby import (
        mesi_two_level_cache_hierarchy,
    )

    return mesi_two_level_cache_hierarchy.MESITwoLevelCacheHierarchy(
        l1i_size="16KiB",
        l1i_assoc=4,
        l1d_size="16KiB",
        l1d_assoc=4,
        l2_size="256KiB",
        l2_assoc=8,
        num_l2_banks=2,
    )


def _scr1_core(
    library: str,
    image: str,
    hart_id: int,
    error_ranges: list[AddrRange],
) -> RtlCore:
    config = json.dumps(
        {"instance_name": f"gem5-scr1-{hart_id}"},
        separators=(",", ":"),
    )
    return RtlCore(
        library=library,
        model_config=config,
        image=image,
        image_format="elf",
        image_bus="instruction",
        initiator_bus_names=["instruction", "data"],
        instruction_bus_name="instruction",
        data_bus_name="data",
        interrupt_input_names=SCR1_INTERRUPT_INPUTS,
        reset_input_names=SCR1_RESET_INPUTS,
        reset_output_names=["sys_rst_n_o"],
        io_input_names=SCR1_IO_INPUTS,
        io_input_values=[
            str(hart_id),
            "0x53435231",
            "0",
            "0",
            "0",
            "0",
            "0",
        ],
        io_output_names=["sys_rdc_qlfy_o", "tdo", "tdo_en"],
        initial_reset_cycles=10,
        error_ranges=error_ranges,
    )


def create_system(
    *,
    library: str,
    image: str,
    memory_system: str,
    clock: str,
    validation: str = "idle",
) -> TestBoard:
    """Create the common system used by classic and Ruby validation."""

    # Ruby cannot route an unmapped address to a default responder. For its
    # error test, complete this range in the neutral packet backend instead.
    error_ranges = (
        [AddrRange(start=0x01000000, size=4096)]
        if memory_system == "ruby" and validation == "error"
        else []
    )
    processor = RtlProcessor(
        cores=[
            _scr1_core(library, image, hart, error_ranges)
            for hart in range(2)
        ]
    )
    memory = SingleChannelSimpleMemory(
        latency="20ns",
        latency_var="0ns",
        bandwidth="12.8GiB/s",
        size="16MiB",
    )
    board = TestBoard(
        clk_freq=clock,
        generator=processor,
        memory=memory,
        cache_hierarchy=_cache_hierarchy(memory_system),
    )
    board.validation_controller = RtlCosimTestController(
        cores=[core.rtl for core in processor.get_cores()],
        mode=validation if validation in ("interrupt", "reset") else "idle",
        poll_interval=10,
        pulse_cycles=20,
        timeout_cycles=2_000_000,
    )
    if validation == "interrupt":
        soft_irq = SCR1_INTERRUPT_INPUTS.index("soft_irq")
        processor.get_cores()[0].rtl.interrupt_inputs[soft_irq] = (
            board.validation_controller.interrupt_outputs[0]
        )
    elif validation == "reset":
        cpu_reset = SCR1_RESET_INPUTS.index("cpu_rst_n")
        processor.get_cores()[0].rtl.reset_inputs[cpu_reset] = (
            board.validation_controller.reset_outputs[0]
        )
    return board


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument(
        "--memory-system",
        choices=("classic", "ruby"),
        default="classic",
    )
    parser.add_argument("--clock", default="50MHz")
    parser.add_argument(
        "--validation",
        choices=("idle", "interrupt", "reset", "error"),
        default="idle",
    )
    parser.add_argument("--max-ticks", type=int, default=10_000_000_000)
    return parser.parse_args()


def main() -> None:
    args = _parse_args()
    if not args.library.is_file():
        raise FileNotFoundError(f"vendor library not found: {args.library}")
    if not args.image.is_file():
        raise FileNotFoundError(f"program image not found: {args.image}")

    board = create_system(
        library=str(args.library.resolve()),
        image=str(args.image.resolve()),
        memory_system=args.memory_system,
        clock=args.clock,
        validation=args.validation,
    )
    simulator = Simulator(board=board, max_ticks=args.max_ticks)
    simulator.run()
    cause = simulator.get_last_exit_event_cause()
    status = simulator.get_last_exit_event_code()
    if cause != "rtl-cosim validation passed" or status != 0:
        raise RuntimeError(
            f"RTL co-simulation failed: cause={cause!r}, status={status}"
        )
    print(f"RTL_COSIM_PASS memory_system={args.memory_system}")


if __name__ == "__m5_main__":
    main()
