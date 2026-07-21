# Copyright (c) 2026 The gem5 Authors
# SPDX-License-Identifier: BSD-3-Clause

"""Run one PULP C910 RTL core against one gem5 memory controller."""

import argparse
import json
from pathlib import Path

import m5
from m5.objects import (
    AddrRange,
    DDR3_1600_8x8,
    MemCtrl,
    NoncoherentXBar,
    Root,
    RtlCoreSimObject,
    RtlCosimTestController,
    SrcClockDomain,
    System,
    VoltageDomain,
)


C910_RESET_INPUTS = ["rst_ni", "jtag_trst_ni"]
C910_INTERRUPT_INPUTS = ["ipi_i", "time_irq_i"] + [
    f"plic_hartx_mint_req_i[{index}]" for index in range(2)
] + [
    f"plic_hartx_sint_req_i[{index}]" for index in range(2)
] + [
    f"ext_int_i[{index}]" for index in range(40)
]
C910_IO_INPUTS = [
    "rtc_i",
    "debug_req_i",
    "jtag_tck_i",
    "jtag_tdi_i",
    "jtag_tms_i",
]
C910_IO_OUTPUTS = ["jtag_tdo_o", "jtag_tdo_en_o", "lpmd_b_o"]
SIGNATURE_ADDRESS = 0x01800020
EXPECTED_SIGNATURE = 0xDC2EFB8ACC3994FF


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--clock", default="50MHz")
    parser.add_argument(
        "--expected-signature",
        type=lambda value: int(value, 0),
        default=EXPECTED_SIGNATURE,
    )
    parser.add_argument("--max-ticks", type=int, default=100_000_000_000)
    return parser.parse_args()


def create_system(
    *, library: str, image: str, clock: str, expected_signature: int
) -> System:
    """Create a core-neutral, direct-memory RTL co-simulation system."""

    system = System()
    system.clk_domain = SrcClockDomain(
        clock=clock, voltage_domain=VoltageDomain()
    )
    system.mem_mode = "timing"
    system.mem_ranges = [AddrRange(start=0, size="64MiB")]
    system.membus = NoncoherentXBar(
        width=16,
        frontend_latency=1,
        forward_latency=0,
        response_latency=1,
    )
    system.system_port = system.membus.cpu_side_ports

    system.mem_ctrl = MemCtrl(
        dram=DDR3_1600_8x8(
            range=system.mem_ranges[0],
            device_size="8MiB",
            ranks_per_channel=1,
        )
    )
    system.mem_ctrl.port = system.membus.mem_side_ports

    system.rtl_core = RtlCoreSimObject(
        library=library,
        model_config=json.dumps(
            {"instance_name": "gem5-c910-0"}, separators=(",", ":")
        ),
        image=image,
        image_format="elf",
        image_bus="memory",
        initiator_bus_names=["memory"],
        interrupt_input_names=C910_INTERRUPT_INPUTS,
        reset_input_names=C910_RESET_INPUTS,
        io_input_names=C910_IO_INPUTS,
        io_input_values=["0"] * len(C910_IO_INPUTS),
        io_output_names=C910_IO_OUTPUTS,
        initial_reset_cycles=10,
    )
    system.rtl_core.initiator_ports = system.membus.cpu_side_ports

    system.validation_controller = RtlCosimTestController(
        cores=[system.rtl_core],
        mode="idle",
        poll_interval=10,
        timeout_cycles=2_000_000,
        signature_address=SIGNATURE_ADDRESS,
        expected_signature=list(expected_signature.to_bytes(8, "little")),
    )
    return system


def main() -> None:
    args = _parse_args()
    if not args.library.is_file():
        raise FileNotFoundError(f"vendor library not found: {args.library}")
    if not args.image.is_file():
        raise FileNotFoundError(f"program image not found: {args.image}")
    if not 0 <= args.expected_signature < 1 << 64:
        raise ValueError("expected signature must fit in 64 bits")

    system = create_system(
        library=str(args.library.resolve()),
        image=str(args.image.resolve()),
        clock=args.clock,
        expected_signature=args.expected_signature,
    )
    root = Root(full_system=False, system=system)
    m5.instantiate()
    event = m5.simulate(args.max_ticks)
    cause = event.getCause()
    status = event.getCode()
    if cause != "rtl-cosim validation passed" or status != 0:
        raise RuntimeError(
            f"RTL co-simulation failed: cause={cause!r}, status={status}"
        )
    print("RTL_COSIM_C910_PASS")


if __name__ == "__m5_main__":
    main()
