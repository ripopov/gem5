# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
Top-level configuration for the CHI direct-injection testbench.

Differences from v1:
  * Each tile hosts a ChiDriverNode (a CHIGenericController subclass)
    instead of a ChiSeqDriver (a ClockedObject with a RequestPort).
  * There is no RubySequencer; the driver speaks CHI directly to the
    colocated tile cache controller via the mesh.
  * Scenarios build ChiDriverNode lists rather than ChiSeqDriver lists.

Everything else — mesh topology (`rbook_4x4.py`), HN-F/SN-F/MN
configuration, address planning, barrier/event-bus wiring — is shared
with v1.
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

# --- path setup -------------------------------------------------------------

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_TB_DIR = os.path.dirname(_THIS_DIR)
_GEM5_CONFIGS = os.path.abspath(
    os.path.join(_THIS_DIR, "..", "..", "..", "..", "configs")
)
addToPath(_GEM5_CONFIGS)

from common import Options  # noqa: E402
from ruby import Ruby  # noqa: E402

sys.path.insert(0, _THIS_DIR)
sys.path.insert(0, os.path.join(_TB_DIR, "scenarios"))
from address_planner import (  # noqa: E402
    AddressPlanner,
    MeshLayout,
)

# --- argument parsing -------------------------------------------------------

parser = argparse.ArgumentParser(
    formatter_class=argparse.ArgumentDefaultsHelpFormatter
)
Options.addNoISAOptions(parser)

parser.add_argument(
    "--scenario",
    default="idle",
    help="Name of the scenario module under scenarios/",
)
parser.add_argument(
    "--scenario-iterations",
    type=int,
    default=1,
    help="Passed through to the scenario's build() function",
)
parser.add_argument(
    "--rn-mode",
    choices=["rni", "rnf_l2"],
    default="rnf_l2",
    help="Per-tile CHI cache-controller mode. "
    "'rnf_l2': tile is a coherent L2-sized leaf cache; "
    "'rni': tile is a cache-less pass-through. "
    "In v2 the Driver is always a separate CHI node on the same "
    "mesh router as the tile, regardless of rn-mode.",
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


# --- scenario resolution ----------------------------------------------------


def _load_scenario(name: str):
    path = os.path.join(_TB_DIR, "scenarios", f"{name}.py")
    if not os.path.isfile(path):
        m5.fatal(f"Scenario '{name}' not found at {path}")
    spec = importlib.util.spec_from_file_location(
        f"chi_tb_gem5_v2_scenario_{name}", path
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


# --- system shell -----------------------------------------------------------

system = System(
    mem_ranges=[AddrRange(args.mem_size)],
    cache_line_size=64,
)

system.voltage_domain = VoltageDomain(voltage=args.sys_voltage)
system.clk_domain = SrcClockDomain(
    clock=args.sys_clock, voltage_domain=system.voltage_domain
)
system.mem_mode = "timing"

# Scenario returns one ChiDriverNode per tile. The drivers are
# CHIGenericController subclasses (and therefore ClockedObjects), so
# they can populate system.cpu directly; CHI.py only uses the list for
# its length.
drivers = scenario.build(args, planner)
if len(drivers) != args.num_cpus:
    m5.fatal(
        f"Scenario '{args.scenario}' returned {len(drivers)} drivers "
        f"but --num-cpus={args.num_cpus}"
    )

system.cpu = drivers

# --- Ruby / CHI / Garnet ----------------------------------------------------

# Swap CHI.create_system's request-node factory for our v2 tile. Each
# tile wraps the pre-built ChiDriverNode plus a CHI_TileCacheController
# and connects both to the same mesh router via ExtLink.
from cfg_rn import CHI_Tile_v2  # noqa: E402

system._rnf_gen = CHI_Tile_v2.make_generator(args.rn_mode)

# As in v1, build the MN with no L1Ds — our tiles have no L1D to
# register for DVM snoops. DVM is architecturally idle in RISC-V SE
# anyway.
from ruby import CHI_config as _chi_cfg  # noqa: E402


class _TestbenchMN(_chi_cfg.CHI_MN):
    class NoC_Params(_chi_cfg.CHI_MN.NoC_Params):
        router_list = [0]


def _mn_gen_no_l1d(options, ruby_system, cpus):
    return [_TestbenchMN(ruby_system, l1d_caches=[])]


system._mn_gen = _mn_gen_no_l1d

Ruby.create_system(args, False, system)

system.ruby.clk_domain = SrcClockDomain(
    clock=args.ruby_clock, voltage_domain=system.voltage_domain
)

# --- Instantiate and run ----------------------------------------------------

root = Root(full_system=False, system=system)

m5.instantiate()

exit_event = m5.simulate(args.abs_max_tick)
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
