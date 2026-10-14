# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
"""Binary32 reduction, DMA throughput probe, or DMA protocol test.

The Python-generated boot image loads inputs before simulated execution.
All CPUs/DMA engines run at 1 GHz; the crossbars run at 4 GHz. The 100-cycle
RAM response latency is defined in the host/system clock domain (100 ns), not
in the accelerator's 4 GHz local router domain. Queue modes retain identical
clocks, transport latency and inputs.
"""

import argparse
import json
import struct
import time
from pathlib import Path

from rt_accel_system import build_system

import m5
from m5.objects import (
    AddrRange,
    BadAddr,
)

INPUT_BASE = 0x82000000
INPUT_BYTES = 16 << 20
PATTERN_BASE = INPUT_BASE + INPUT_BYTES


def initialize_memory(path):
    """Write a small ELF header plus contiguous 16 MiB of little-endian 1.0f.

    The second PT_LOAD segment supplies a nonuniform DMA ordering test input.
    An ELF image names physical addresses directly without a sparse RAM dump
    or simulated host stores. Firmware and input segments do not overlap.
    """
    data_offset = 4096
    pattern_bytes = 16 << 10
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    header = struct.pack(
        "<16sHHIQQQIHHHHHH",
        ident,
        2,
        243,
        1,
        0,
        64,
        0,
        0,
        64,
        56,
        2,
        0,
        0,
        0,
    )
    segments = b"".join(
        struct.pack("<IIQQQQQQ", 1, 6, offset, addr, addr, size, size, 4096)
        for offset, addr, size in (
            (data_offset, INPUT_BASE, INPUT_BYTES),
            (data_offset + INPUT_BYTES, PATTERN_BASE, pattern_bytes),
        )
    )
    with path.open("wb") as stream:
        stream.write(header + segments)
        stream.write(bytes(data_offset - stream.tell()))
        stream.write(struct.pack("<f", 1.0) * (INPUT_BYTES // 4))
        stream.write(
            bytes((37 * i + i // 64) & 255 for i in range(pattern_bytes))
        )


def initialize_bootloader(path, firmware, inputs):
    """Append input segments to firmware, retaining its entry and symbols.

    RiscvBareMetal loads through the system physical proxy, which routes each
    cache line to its controller. A SimpleMemory image loader writes only to
    that memory object and cannot initialize interleaved controllers.
    """
    boot = bytearray(firmware.read_bytes())
    image = inputs.read_bytes()
    headers = bytearray()
    for data, offset in ((boot, 0), (image, (len(boot) + 4095) & ~4095)):
        ph_offset = struct.unpack_from("<Q", data, 32)[0]
        ph_size, ph_count = struct.unpack_from("<HH", data, 54)
        assert data[:7] == b"\x7fELF\x02\x01\x01" and ph_size == 56
        for i in range(ph_count):
            segment = list(
                struct.unpack_from("<IIQQQQQQ", data, ph_offset + i * 56)
            )
            segment[2] += offset
            headers.extend(struct.pack("<IIQQQQQQ", *segment))
    boot.extend(bytes(offset - len(boot)))
    boot.extend(image)
    struct.pack_into("<Q", boot, 32, len(boot))
    struct.pack_into("<H", boot, 56, len(headers) // 56)
    boot.extend(headers)
    path.write_bytes(boot)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument(
    "--workload",
    choices=("reduction", "copy", "dma-test", "dma-fault"),
    default="reduction",
)
parser.add_argument("--queues", type=int, default=1)
parser.add_argument("--accelerator-cores", type=int)
parser.add_argument(
    "--accelerator-cpu", choices=("timing", "o3"), default="o3"
)
parser.add_argument(
    "--accelerator-caches", choices=("l1", "none"), default="l1"
)
parser.add_argument("--queue-depth", type=int, default=512)
parser.add_argument("--dma-outstanding", type=int, default=512)
# Five cycles per direction at the fixed 1 GHz CPU/device clock.
parser.add_argument("--transport-delay", default="5ns")
parser.add_argument("--memory-cycles", type=int, default=100)
parser.add_argument(
    "--memory-controllers", type=int, choices=(1, 2, 4), default=4
)
parser.add_argument("--memory-bandwidth", help="Bandwidth per controller")
parser.add_argument("--trace", action="store_true")
args = parser.parse_args()
if args.memory_bandwidth is None:
    # With one controller, 32 GiB/s makes the scalar reduction memory
    # bound. Bandwidth is per controller, including the four-controller
    # default. The separate copy probe provisions a full 64 B/cycle path.
    args.memory_bandwidth = (
        "32GiB/s" if args.workload == "reduction" else "64GiB/s"
    )
args.backend = "gem5"  # Retained in parameters.json
if args.accelerator_cores is None:
    args.accelerator_cores = 16 if args.workload == "reduction" else 1
if not 1 <= args.accelerator_cores <= 16:
    parser.error("accelerator cores must be in [1, 16]")
if args.workload != "reduction" and args.accelerator_cores != 1:
    parser.error("DMA diagnostic workloads require one accelerator")
if args.queues not in (1, args.accelerator_cores + 1):
    parser.error("queues must be 1 or accelerator_cores + 1")
if min(args.queue_depth, args.dma_outstanding, args.memory_cycles) < 1:
    parser.error("queue depth, DMA credits and memory cycles must be positive")
firmware_dir = Path(__file__).resolve().parents[2] / "util/rt_reference/out"
firmware = firmware_dir / "reduction-host.elf"
kernel = firmware_dir / "reduction-accelerator.elf"
if not firmware.is_file() or not kernel.is_file():
    parser.error("build firmware first: make -C util/rt_reference reduction")
outdir = Path(m5.options.outdir).resolve()
outdir.mkdir(parents=True, exist_ok=True)
parameters = outdir / "parameters.bin"
parameters.write_bytes(
    struct.pack(
        "<I",
        ("reduction", "copy", "dma-test", "dma-fault").index(args.workload),
    )
)
(outdir / "parameters.json").write_text(
    json.dumps(vars(args), indent=2) + "\n"
)
initial_image = outdir / "inputs.elf"
initialize_memory(initial_image)
boot_image = outdir / "boot.elf"
initialize_bootloader(boot_image, firmware, initial_image)
m5.ticks.setGlobalFrequency("1THz")
# One cycle in the fixed 1 GHz system clock is exactly 1 ns.
root, system = build_system(
    args,
    boot_image,
    kernel,
    outdir,
    parameters,
    memory_latency=f"{args.memory_cycles}ns",
    memory_bandwidth=args.memory_bandwidth,
    memory_width=64,
    memory_router_clock="4GHz",
    memory_controllers=args.memory_controllers,
    memory_range=(
        AddrRange(0x80000000, size="63MiB")
        if args.workload == "dma-fault"
        else None
    ),
)
if args.workload == "dma-fault":
    # A standard error responder replaces the last MiB of the host aperture.
    # DMA address checks still accept it, so errors occur with packets live.
    system.fault_memory = BadAddr(
        pio_addr=0x83F00000, pio_size=1 << 20, pio_latency="100ns"
    )
    system.fault_memory.pio = system.membus.mem_side_ports
# Copy/protocol results, like descriptors and partials, have no cached aliases.
system.cpu.mmu.pma_checker.uncacheable = [
    AddrRange(0x10000000, size=0x1000 * args.accelerator_cores),
    AddrRange(0x20000000, size=0x1000),
    AddrRange(0x81000000, size="1MiB"),
    AddrRange(0x83100000, size="1MiB"),
]
m5.instantiate()
started = time.perf_counter()
event = m5.simulate()
simulation_seconds = time.perf_counter() - started
(outdir / "simulation-wall.json").write_text(
    json.dumps(
        {
            "wall_seconds": simulation_seconds,
            "start_tick": 0,
            "end_tick": m5.curTick(),
            "boundary": "After instantiate through return from simulate; "
            "boot, "
            "batch, validation and export included; image setup, automatic "
            "exit statistics and cleanup excluded",
        },
        indent=2,
    )
    + "\n"
)
print(f"Stopped at {m5.curTick()}: {event.getCause()}")
if event.getCause() != "m5_exit instruction encountered":
    raise RuntimeError(f"Firmware failed: {event.getCause()}")
expected = "sum.bin" if args.workload == "reduction" else "copy.bin"
if not (outdir / expected).is_file():
    raise RuntimeError(f"Firmware exited without {expected}")
