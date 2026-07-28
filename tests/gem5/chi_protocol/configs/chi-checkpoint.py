# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause

"""Take and restore a checkpoint of a Ruby CHI system.

The board matches ``chi-with-isa.py``: private CHI L1s over SimpleNetwork,
one directory and one memory channel. The workload keeps a buffer dirty in
those caches for its whole run, so a checkpoint taken part-way through has
real writeback work to do and the trace replayed on restore is non-empty.

Run without ``--take-checkpoint-at`` or ``--restore`` for the reference run.
``run_checkpoint_regression.py`` chains the three modes.
"""

import argparse
from pathlib import Path

import m5
from m5.util import fatal

from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.chi.private_l1_cache_hierarchy import (
    PrivateL1CacheHierarchy,
)
from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("binary", help="the RISC-V SE-mode workload to run")
parser.add_argument("--num-cores", type=int, default=1)
parser.add_argument(
    "--checkpoint-path", type=Path, help="checkpoint directory to write/read"
)
parser.add_argument(
    "--take-checkpoint-at",
    type=int,
    help="tick at which to stop and write a checkpoint",
)
parser.add_argument(
    "--restore", action="store_true", help="restore --checkpoint-path"
)
args = parser.parse_args()
if args.checkpoint_path is None and (args.restore or args.take_checkpoint_at):
    parser.error("--checkpoint-path is required to save or restore")

requires(
    isa_required=ISA.RISCV, coherence_protocol_required=CoherenceProtocol.CHI
)

board = SimpleBoard(
    clk_freq="3GHz",
    processor=SimpleProcessor(
        cpu_type=CPUTypes.TIMING, isa=ISA.RISCV, num_cores=args.num_cores
    ),
    memory=SingleChannelDDR3_1600(size="512MiB"),
    cache_hierarchy=PrivateL1CacheHierarchy(size="32KiB", assoc=8),
)
board.set_se_binary_workload(
    BinaryResource(local_path=args.binary),
    checkpoint=args.checkpoint_path if args.restore else None,
)

simulator = Simulator(board=board)

if args.take_checkpoint_at is None:
    simulator.run()
else:
    simulator.run(max_ticks=args.take_checkpoint_at)
    if m5.curTick() < args.take_checkpoint_at:
        fatal(
            "workload ended at tick %d, before the requested checkpoint at "
            "%d; there would be no resumable state left to check",
            m5.curTick(),
            args.take_checkpoint_at,
        )
    args.checkpoint_path.mkdir(parents=True, exist_ok=True)
    simulator.save_checkpoint(args.checkpoint_path)
    print("RUBY-CKPT-SAVED")
