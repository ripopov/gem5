# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
Top-level configuration for the CPU-less CHI/Garnet mesh testbench.

Reuses the Chapter 17 CHI/Garnet/SLICC stack verbatim. Each tile hosts
a ChiSeqDriver (ClockedObject) running a Fiber-backed sequence, wired
directly into a tile-local Ruby sequencer. The `--scenario` flag
selects a registered sequence by name; each scenario module under
`scenarios/` picks a sequence per tile and passes the shared
ChiGem5Barrier / ChiGem5EventBus instances through each driver's
params. `--rn-mode` picks the per-tile CHI attachment (see cfg_rn.py).
"""

from __future__ import annotations

import argparse
import importlib
import importlib.util
import os
import sys

import m5
from m5.defines import buildEnv
from m5.objects import (
    AddrRange,
    Root,
    SrcClockDomain,
    System,
    VoltageDomain,
)
from m5.util import addToPath

# --- path setup --------------------------------------------------------------

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_TB_DIR = os.path.dirname(_THIS_DIR)
_GEM5_CONFIGS = os.path.abspath(
    os.path.join(_THIS_DIR, "..", "..", "..", "..", "configs")
)
addToPath(_GEM5_CONFIGS)

from common import Options  # noqa: E402
from ruby import Ruby  # noqa: E402

# local imports
sys.path.insert(0, _THIS_DIR)
sys.path.insert(0, os.path.join(_TB_DIR, "scenarios"))
from address_planner import (  # noqa: E402
    AddressPlanner,
    MeshLayout,
)

# --- argument parsing --------------------------------------------------------

parser = argparse.ArgumentParser(
    formatter_class=argparse.ArgumentDefaultsHelpFormatter
)
Options.addNoISAOptions(parser)

parser.add_argument(
    "--scenario",
    default="memset",
    help="Name of the scenario module under scenarios/",
)
parser.add_argument(
    "--scenario-iterations",
    type=int,
    default=1,
    help="Passed through to the scenario's build() function",
)
parser.add_argument(
    "--deadlock-threshold",
    type=int,
    default=5_000_000,
    help="Sequencer deadlock threshold (cycles); testbench traffic can "
    "be sparse, so we raise this above the default",
)
parser.add_argument(
    "--rn-mode",
    choices=["rni", "rnf_l2"],
    default="rnf_l2",
    help="Per-tile CHI request-node mode. "
    "'rnf_l2': Seq -> coherent L2-sized leaf cache -> mesh; "
    "'rni': Seq -> cache-less DMA controller -> mesh. "
    "Neither mode instantiates the side router that CHI_RNF normally "
    "adds.",
)

if buildEnv["PROTOCOL"] == "MULTIPLE" and not any(
    a.startswith("--protocol") for a in sys.argv
):
    sys.argv.append("--protocol=CHI")

Ruby.define_options(parser)

parser.set_defaults(
    num_cpus=16,
    num_l3caches=16,
    num_dirs=2,
    topology="CustomMesh",
    network="garnet",
    chi_config=os.path.join(
        _GEM5_CONFIGS, "example", "noc_config", "rbook_4x4.py"
    ),
    mem_size="512MiB",
)

args = parser.parse_args()


# --- scenario resolution -----------------------------------------------------


def _load_scenario(name: str):
    path = os.path.join(_TB_DIR, "scenarios", f"{name}.py")
    if not os.path.isfile(path):
        m5.fatal(f"Scenario '{name}' not found at {path}")
    spec = importlib.util.spec_from_file_location(
        f"chi_tb_gem5_scenario_{name}", path
    )
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


scenario = _load_scenario(args.scenario)

planner = AddressPlanner(
    MeshLayout(
        cache_line_size=64,
        num_hnfs=args.num_l3caches,
        num_rows=4,
        num_cols=4,
    )
)


# --- system shell ------------------------------------------------------------

system = System(
    mem_ranges=[AddrRange(args.mem_size)],
    cache_line_size=64,
)

system.voltage_domain = VoltageDomain(voltage=args.sys_voltage)
system.clk_domain = SrcClockDomain(
    clock=args.sys_clock, voltage_domain=system.voltage_domain
)
system.mem_mode = "timing"

# Scenario returns one ChiSeqDriver per tile. ChiSeqDriver is itself
# a ClockedObject, so it sits at system.cpu[i] without any adapter.
drivers = scenario.build(args, planner)
if len(drivers) != args.num_cpus:
    m5.fatal(
        f"Scenario '{args.scenario}' returned {len(drivers)} drivers "
        f"but --num-cpus={args.num_cpus}"
    )

system.cpu = drivers

# --- Ruby / CHI / Garnet -----------------------------------------------------

# Swap CHI.create_system's request-node factory for our direct-mesh
# Tile (see configs/ruby/CHI.py:125 for the _rnf_gen hook). Every tile
# becomes `RubySequencer -> CHI_TileCacheController -> mesh router`,
# with no L1, no L2-in-RNF, and no side router — the controller is
# parameterized by --rn-mode to act as either a cache-less DMA node
# (rni) or a coherent leaf cache (rnf_l2).
from cfg_rn import CHI_Tile  # noqa: E402

system._rnf_gen = CHI_Tile.make_generator(args.rn_mode)

# Build the Misc Node (DVM coordinator) with no upstream L1Ds. The
# stock CHI_MN.generate at configs/ruby/CHI_config.py:728 collects
# `cpu.l1d` from every CPU to register DVM snoop targets, but our
# tiles have no L1D. DVM is architecturally idle in RISC-V SE mode
# anyway, so an empty upstream destination list is fine.
#
# We subclass locally to pin `router_list = [0]`; CustomMesh reads
# `type(n).NoC_Params.router_list` and our noc_config's CHI_MN
# shadow class is not reachable through the stock CHI_MN type.
from ruby import CHI_config as _chi_cfg  # noqa: E402


class _TestbenchMN(_chi_cfg.CHI_MN):
    class NoC_Params(_chi_cfg.CHI_MN.NoC_Params):
        router_list = [0]


def _mn_gen_no_l1d(options, ruby_system, cpus):
    return [_TestbenchMN(ruby_system, l1d_caches=[])]


system._mn_gen = _mn_gen_no_l1d

Ruby.create_system(args, False, system)
assert args.num_cpus == len(system.ruby._cpu_ports)

system.ruby.clk_domain = SrcClockDomain(
    clock=args.ruby_clock, voltage_domain=system.voltage_domain
)

# Raise deadlock thresholds; testbench traffic is sparse by design.
for i in range(args.num_cpus):
    system.ruby._cpu_ports[i].deadlock_threshold = args.deadlock_threshold

# Wire each driver's RequestPort to its RN-F data sequencer's in_ports.
# CPUSequencerWrapper.in_ports maps to the data sequencer (see
# configs/ruby/CHI_config.py:457).
for i, drv in enumerate(system.cpu):
    drv.clk_domain = system.clk_domain
    drv.port = system.ruby._cpu_ports[i].in_ports

# --- Instantiate and run -----------------------------------------------------

root = Root(full_system=False, system=system)

m5.instantiate()

exit_event = m5.simulate(args.abs_max_tick)
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
