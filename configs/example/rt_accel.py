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

"""Bare-metal host reference or configurable scalar gem5 accelerators.

Build the resident firmware with `make -C util/rt_reference` first.
"""

import argparse
import json
import struct
from pathlib import Path

from rt_accel_system import build_system

import m5


def unsigned(value):
    return int(value, 0)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--backend", choices=("reference", "gem5"), default="gem5")
parser.add_argument("--accelerator-cores", type=int, default=8)
parser.add_argument(
    "--queues",
    type=int,
    default=1,
    help="One shared queue or accelerator-cores + 1 separate queues",
)
parser.add_argument(
    "--accelerator-cpu",
    choices=("timing", "o3"),
    default="timing",
    help="Accelerator core model: TimingSimple (default) or O3",
)
parser.add_argument("--host-firmware", type=Path)
parser.add_argument(
    "--host-render",
    action="store_true",
    help="Render on the host while gem5 accelerator CPUs wait in WFI",
)
parser.add_argument(
    "--accelerator-caches", choices=("l1", "none"), default="l1"
)
parser.add_argument("--accelerator-firmware", type=Path)
parser.add_argument("--queue-depth", type=int, default=16)
parser.add_argument("--dma-outstanding", type=int, default=256)
parser.add_argument(
    "--memory-controllers", type=int, choices=(1, 2, 4), default=4
)
parser.add_argument("--trace", action="store_true")
# Five cycles per direction at the fixed 1 GHz CPU/device clock.
parser.add_argument("--transport-delay", default="5ns")
parser.add_argument(
    "--render-mode", "--render_mode", type=int, choices=(0, 1), default=0
)
parser.add_argument(
    "--samples-per-pixel", "--samples_per_pixel", type=int, default=None
)
parser.add_argument(
    "--max-secondary-depth", "--max_secondary_depth", type=int, default=None
)
parser.add_argument("--seed", type=unsigned, default=1)
args = parser.parse_args()
if args.host_render and args.backend != "gem5":
    parser.error("host_render requires backend=gem5")
if not 1 <= args.accelerator_cores <= 64:
    parser.error("accelerator_cores must be in [1, 64]")
if args.queues not in (1, args.accelerator_cores + 1):
    parser.error("queues must be 1 or accelerator_cores + 1")
if args.samples_per_pixel is None:
    args.samples_per_pixel = 1 if args.render_mode == 1 else 4
if args.max_secondary_depth is None:
    args.max_secondary_depth = 0 if args.render_mode == 1 else 4
if args.dma_outstanding < 1:
    parser.error("dma_outstanding must be positive")
if args.queue_depth < 1:
    parser.error("queue_depth must be positive")
if not 1 <= args.samples_per_pixel <= 4096:
    parser.error("samples_per_pixel must be in [1, 4096]")
if not 0 <= args.max_secondary_depth <= 32:
    parser.error("max_secondary_depth must be in [0, 32]")
if not 0 <= args.seed < 2**64:
    parser.error("seed must be an unsigned 64-bit integer")
if args.render_mode == 1 and (
    args.samples_per_pixel != 1 or args.max_secondary_depth != 0
):
    parser.error("smoke mode requires samples_per_pixel=1 and depth=0")

firmware = Path(__file__).resolve().parents[2] / (
    "util/rt_reference/out/"
    + (
        "host-render.elf"
        if args.host_render
        else "host-gem5.elf" if args.backend == "gem5" else "host.elf"
    )
)
if args.host_firmware:
    firmware = args.host_firmware.resolve()
if not firmware.is_file():
    parser.error("build firmware first: make -C util/rt_reference")
outdir = Path(m5.options.outdir).resolve()
outdir.mkdir(parents=True, exist_ok=True)
parameters = outdir / "parameters.bin"
parameters.write_bytes(
    struct.pack(
        "<IIIIQ",
        0x52545031,
        args.render_mode,
        args.samples_per_pixel,
        args.max_secondary_depth,
        args.seed,
    )
)
(outdir / "parameters.json").write_text(
    json.dumps(vars(args), indent=2, default=str) + "\n"
)

m5.ticks.setGlobalFrequency("1THz")
kernel = (
    args.accelerator_firmware.resolve()
    if args.accelerator_firmware
    else firmware.with_name("accelerator.elf")
)
if args.backend == "gem5" and not kernel.is_file():
    parser.error("build accelerator.elf first")
root, system = build_system(
    args,
    firmware,
    kernel,
    outdir,
    parameters,
    memory_controllers=args.memory_controllers,
)
m5.instantiate()
while True:
    event = m5.simulate(10_000_000_000)
    if event.getCause() != "simulate() limit reached":
        break
    print(
        f"Rendering: tick {m5.curTick()}, "
        f"instructions {system.cpu.totalInsts()}",
        flush=True,
    )
print(f"Stopped at {m5.curTick()}: {event.getCause()}")
if event.getCause() != "m5_exit instruction encountered":
    raise RuntimeError(f"Firmware failed: {event.getCause()}")
if not (outdir / "image.bmp").is_file():
    raise RuntimeError("Firmware exited without exporting image.bmp")
