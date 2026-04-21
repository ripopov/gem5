# Copyright (c) 2026 ripopov
# SPDX-License-Identifier: BSD-3-Clause
"""
Top-level configuration for the CPU-less CHI-Garnet Mesh testbench.

The system shell is identical to rbook_mesh_config.py (the Chapter 17
system): 4x4 CHI mesh, 16 RN-F tiles, 16 HNF slices, 2 SN-F
controllers at diagonal corners.  The only change is that each tile's
"CPU" is a SystemC SC_MODULE driver, attached to the RN-F sequencer
through a TlmToGem5Bridge64.  Scenario selection (`--scenario=<name>`)
picks which driver subclass goes at each tile.

Reference shells this is based on:
- configs/example/rbook_mesh_config.py (Chapter 17 system wiring)
- configs/example/ruby_mem_test.py     (CPU-less tester pattern)
- configs/example/dramsys.py           (SystemC_Kernel + TLM bridge)
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
    ChiTileSlot,
    Root,
    SrcClockDomain,
    System,
    SystemC_Kernel,
    TlmToGem5Bridge64,
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
    default="smoke_read",
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
        f"chi_testbench_scenario_{name}", path
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

# The scenario returns one SystemC driver per tile. SystemC_ScModule
# objects cannot serve as cpu slots because their C++ base is
# sc_module, not SimObject, so their RubySequencer children would
# collide at global stats scope. Mount the drivers as siblings and
# use ChiTileSlot placeholders as the cpu-slot parents.
drivers = scenario.build(args, planner)
if len(drivers) != args.num_cpus:
    m5.fatal(
        f"Scenario '{args.scenario}' returned {len(drivers)} drivers "
        f"but --num-cpus={args.num_cpus}"
    )

system.cpu = [ChiTileSlot() for _ in range(args.num_cpus)]
system.drivers = drivers

# --- Ruby / CHI / Garnet -----------------------------------------------------

Ruby.create_system(args, False, system)
assert args.num_cpus == len(system.ruby._cpu_ports)

system.ruby.clk_domain = SrcClockDomain(
    clock=args.ruby_clock, voltage_domain=system.voltage_domain
)

# Raise deadlock thresholds; testbench traffic is sparse by design.
for i in range(args.num_cpus):
    system.ruby._cpu_ports[i].deadlock_threshold = args.deadlock_threshold

# --- TLM bridges per tile ----------------------------------------------------

# One bridge per tile. Each bridge links a SystemC driver's TLM
# initiator socket to the RN-F data sequencer's gem5 in_ports.
system.bridges = [TlmToGem5Bridge64() for _ in range(args.num_cpus)]
for i, (drv, br) in enumerate(zip(drivers, system.bridges)):
    br.gem5 = system.ruby._cpu_ports[i].in_ports
    drv.iSocket = br.tlm

# --- SystemC kernel ----------------------------------------------------------

# Kernel must be a child of Root, not System, to avoid a reference cycle
# (kernel.system would otherwise point back into its own parent).
systemc_kernel = SystemC_Kernel(system=system)

# --- Instantiate and run -----------------------------------------------------

root = Root(
    full_system=False,
    system=system,
    systemc_kernel=systemc_kernel,
)

m5.instantiate()

exit_event = m5.simulate(args.abs_max_tick)
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
