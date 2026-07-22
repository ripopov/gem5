# Copyright (c) 2026 The gem5 Authors
# SPDX-License-Identifier: BSD-3-Clause

"""Switch one RISC-V fast CPU into a preconnected PULP C910 RTL CPU."""

import argparse
import json
from pathlib import Path

import m5
from m5.defines import buildEnv
from m5.objects import (
    AddrRange,
    DDR3_1600_8x8,
    MemCtrl,
    RiscvAtomicSimpleCPU,
    RiscvBareMetal,
    RiscvSystem,
    Root,
    RtlCoreSimObject,
    RtlCosimTestController,
    RtlCpuSimObject,
    SrcClockDomain,
    SystemXBar,
    VoltageDomain,
)

from c910_single_core import (
    C910_INTERRUPT_INPUTS,
    C910_IO_INPUTS,
    C910_IO_OUTPUTS,
    C910_RESET_INPUTS,
)


DUMP_ADDRESS = 0x01820000
PASS_SIGNATURES = {
    "integer": 0x600DC91051000001,
    "compute": 0x600DC91051000002,
    "fp": 0x600DC91051000003,
    "trap": 0x600DC91051000004,
    "sv39": 0x600DC91051000005,
    "user": 0x600DC9105100000A,
    "atomic": 0x600DC91051000006,
    "interrupt": 0x600DC91051000007,
    "lrsc": 0x600DC91051000008,
    "external_interrupt": 0x600DC91051000009,
}


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", required=True, type=Path)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument(
        "--source-cpu", choices=("atomic", "jit"), default="atomic"
    )
    parser.add_argument("--jit-backend", type=Path)
    parser.add_argument(
        "--case", choices=tuple(PASS_SIGNATURES), default="integer"
    )
    parser.add_argument("--clock", default="50MHz")
    parser.add_argument("--max-ticks", type=int, default=200_000_000_000)
    return parser.parse_args()


def _integer_signature() -> bytes:
    values = [0]
    values.extend(0x1111000000000000 + index for index in range(1, 31))
    # gem5's RISC-V m5 instruction format defines a0 and a1 as result
    # operands. m5_switch_cpu has no return value, so the switch instruction
    # itself architecturally writes both registers to zero before handover.
    values[10] = 0
    values[11] = 0
    values.append(DUMP_ADDRESS)
    values.append(PASS_SIGNATURES["integer"])
    return b"".join(value.to_bytes(8, "little") for value in values)


def _fp_signature() -> bytes:
    values = [0x3FF0000000000000 + index for index in range(32)]
    values.extend(
        (
            0x61,
            0x400E000000000000,
            0x400B000000000000,
            PASS_SIGNATURES["fp"],
        )
    )
    return b"".join(value.to_bytes(8, "little") for value in values)


def _expected_signature(case: str) -> bytes:
    if case == "integer":
        return _integer_signature()
    if case == "fp":
        return _fp_signature()
    return PASS_SIGNATURES[case].to_bytes(8, "little")


def create_system(
    *,
    library: str,
    image: str,
    clock: str,
    case: str,
    source_cpu: str = "atomic",
    jit_backend: str = "",
) -> RiscvSystem:
    system = RiscvSystem()
    system.clk_domain = SrcClockDomain(
        clock=clock, voltage_domain=VoltageDomain()
    )
    system.mem_mode = (
        "atomic_noncaching" if source_cpu == "jit" else "atomic"
    )
    # OpenC910's fixed physical-memory attributes mark 0x1000_0000 through
    # 0x13ff_ffff cacheable. Include that window so RTL AMOs can be tested on
    # architecturally valid cacheable memory.
    system.mem_ranges = [AddrRange(start=0, size="512MiB")]
    system.membus = SystemXBar(width=16)
    system.system_port = system.membus.cpu_side_ports

    system.mem_ctrl = MemCtrl(
        dram=DDR3_1600_8x8(
            range=system.mem_ranges[0],
            device_size="64MiB",
            ranks_per_channel=1,
        )
    )
    system.mem_ctrl.port = system.membus.mem_side_ports
    system.workload = RiscvBareMetal(bootloader=image)

    if source_cpu == "jit":
        if not buildEnv["USE_JITCPU"]:
            raise RuntimeError(
                "--source-cpu jit requires a gem5 build with USE_JITCPU=y"
            )
        from m5.objects import RiscvJitCPU

        system.fast_cpu = RiscvJitCPU(
            cpu_id=0,
            backend_path=jit_backend,
            batch_size=256,
        )
    else:
        system.fast_cpu = RiscvAtomicSimpleCPU(cpu_id=0)
    system.fast_cpu.icache_port = system.membus.cpu_side_ports
    system.fast_cpu.dcache_port = system.membus.cpu_side_ports
    system.fast_cpu.mmu.connectWalkerPorts(
        system.membus.cpu_side_ports, system.membus.cpu_side_ports
    )
    system.fast_cpu.createInterruptController()
    system.fast_cpu.createThreads()
    # riscv64/v1 intentionally excludes vector registers. Keep both gem5
    # architectural contexts on the scalar ISA profile for an exact handover.
    system.fast_cpu.isa[0].enable_rvv = False

    system.rtl_core = RtlCoreSimObject(
        library=library,
        model_config=json.dumps(
            {"instance_name": "gem5-c910-switch"}, separators=(",", ":")
        ),
        defer_startup=True,
        initiator_bus_names=["memory"],
        interrupt_input_names=C910_INTERRUPT_INPUTS,
        reset_input_names=C910_RESET_INPUTS,
        io_input_names=C910_IO_INPUTS,
        io_input_values=["0"] * len(C910_IO_INPUTS),
        io_output_names=C910_IO_OUTPUTS,
        initial_reset_cycles=10,
    )
    system.rtl_core.initiator_ports = system.membus.cpu_side_ports

    system.rtl_cpu = RtlCpuSimObject(
        cpu_id=0,
        switched_out=True,
        rtl_core=system.rtl_core,
    )
    system.rtl_cpu.createInterruptController()
    system.rtl_cpu.createThreads()
    system.rtl_cpu.isa[0].enable_rvv = False

    system.validation_controller = RtlCosimTestController(
        cores=[system.rtl_core],
        mode="interrupt"
        if case in ("interrupt", "external_interrupt")
        else "idle",
        poll_interval=10,
        # C910 synchronizes raw wake inputs into its low-power controller;
        # hold the level long enough for that path and the trap entry.
        pulse_cycles=1000,
        timeout_cycles=2_000_000,
        signature_address=DUMP_ADDRESS,
        expected_signature=list(_expected_signature(case)),
    )
    if case == "interrupt":
        time_irq = C910_INTERRUPT_INPUTS.index("time_irq_i")
        system.rtl_core.interrupt_inputs[time_irq] = (
            system.validation_controller.interrupt_outputs[0]
        )
    elif case == "external_interrupt":
        external_irq = C910_INTERRUPT_INPUTS.index(
            "plic_hartx_mint_req_i[0]"
        )
        system.rtl_core.interrupt_inputs[external_irq] = (
            system.validation_controller.interrupt_outputs[0]
        )
    return system


def main() -> None:
    args = _parse_args()
    if not args.library.is_file():
        raise FileNotFoundError(f"vendor library not found: {args.library}")
    if not args.image.is_file():
        raise FileNotFoundError(f"program image not found: {args.image}")
    if args.source_cpu == "jit" and (
        args.jit_backend is None or not args.jit_backend.is_file()
    ):
        raise FileNotFoundError(
            f"JitCPU backend not found: {args.jit_backend}"
        )

    system = create_system(
        library=str(args.library.resolve()),
        image=str(args.image.resolve()),
        clock=args.clock,
        case=args.case,
        source_cpu=args.source_cpu,
        jit_backend=(
            str(args.jit_backend.resolve())
            if args.jit_backend is not None
            else ""
        ),
    )
    Root(full_system=True, system=system)
    m5.instantiate()

    event = m5.simulate(args.max_ticks)
    if event.getCause() != "switchcpu":
        raise RuntimeError(
            "AtomicSimpleCPU did not reach the switch point: "
            f"cause={event.getCause()!r}, status={event.getCode()}"
        )
    m5.switchCpus(system, [(system.fast_cpu, system.rtl_cpu)])

    remaining = max(1, args.max_ticks - m5.curTick())
    event = m5.simulate(remaining)
    if event.getCause() != "rtl-cosim validation passed" or event.getCode():
        raise RuntimeError(
            "RTL continuation failed: "
            f"cause={event.getCause()!r}, status={event.getCode()}"
        )
    print(f"RTL_COSIM_CPU_SWITCH_PASS case={args.case}")
    if args.source_cpu == "jit":
        print(f"RTL_COSIM_JIT_TO_C910_PASS case={args.case}")


if __name__ == "__m5_main__":
    main()
