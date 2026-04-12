# System configuration for the ruby-book 4x4 CHI mesh final project
# (Chapter 17, Stages 1-2).
#
# Creates 16 RISC-V TimingSimpleCPU cores wired through Ruby/CHI to a 4x4
# Garnet mesh with 16 HN-F (LLC slices) and 2 SN-F (DDR controllers) at
# diagonally opposite corners.
#
# Usage:
#   ./build/RISCV/gem5.opt -d m5out/rbook-test-$(date +%Y%m%d-%H%M%S) \
#       configs/example/rbook_mesh_config.py --cmd=<path-to-binary>

import argparse
import os
import shlex
import sys

import m5
from m5.objects import *
from m5.util import addToPath

addToPath("../")

from common import Options
from ruby import Ruby

# --- Argument parsing --------------------------------------------------------

parser = argparse.ArgumentParser(
    description="16-core CHI mesh system for the ruby-book final project"
)
Options.addCommonOptions(parser)
Options.addSEOptions(parser)
parser.add_argument(
    "--trace-output",
    type=str,
    default=None,
    help="Enable FTR text tracing with the given output basename",
)

# This script only supports CHI. MULTIPLE.define_options() scans sys.argv for
# --protocol before parse_args() runs, so inject it unconditionally.
if not any(a.startswith("--protocol") for a in sys.argv):
    sys.argv.append("--protocol=CHI")

Ruby.define_options(parser)

# Hardcode the mesh-specific defaults so the user only needs --cmd.
parser.set_defaults(
    num_cpus=16,
    num_l3caches=16,
    num_dirs=2,
    topology="CustomMesh",
    network="garnet",
    chi_config=os.path.join(
        os.path.dirname(__file__), "noc_config", "rbook_4x4.py"
    ),
    cpu_type="RiscvTimingSimpleCPU",
    mem_size="512MiB",
)

args = parser.parse_args()

if not args.cmd:
    print(
        "Error: --cmd is required (path to the SE-mode binary)",
        file=sys.stderr,
    )
    sys.exit(1)

# --- System shell ------------------------------------------------------------

system = System(
    cpu=[TimingSimpleCPU(cpu_id=i) for i in range(args.num_cpus)],
    mem_mode="timing",
    mem_ranges=[AddrRange(args.mem_size)],
    cache_line_size=64,
)

system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(
    clock="2GHz", voltage_domain=system.voltage_domain
)

# --- SE workload (single Process shared by all CPUs) -------------------------

binary_path = args.cmd
cmd = [binary_path]
if args.options:
    cmd.extend(shlex.split(args.options))

process = Process(
    pid=100,
    executable=binary_path,
    cmd=cmd,
    cwd=os.getcwd(),
)

for cpu in system.cpu:
    cpu.workload = process
    cpu.createThreads()

system.workload = SEWorkload.init_compatible(binary_path)

# --- Ruby / CHI / Garnet -----------------------------------------------------

Ruby.create_system(args, False, system)
assert args.num_cpus == len(system.ruby._cpu_ports)

system.ruby.clk_domain = SrcClockDomain(
    clock=args.ruby_clock, voltage_domain=system.voltage_domain
)

for i in range(args.num_cpus):
    system.cpu[i].createInterruptController()
    system.ruby._cpu_ports[i].connectCpuPorts(system.cpu[i])

if args.trace_output is not None:
    system.ftr_trace = FtrTrace(
        output_format="text", output_file=args.trace_output
    )

# --- Instantiate and run -----------------------------------------------------

root = Root(full_system=False, system=system)
root.trace = FstTrace(
    trace_file="trace.fst",
    start_active=True,
    stat_sample_period=5000000,  # ~10,000 CPU cycles at 2GHz
)
m5.instantiate()
exit_event = m5.simulate()

print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
