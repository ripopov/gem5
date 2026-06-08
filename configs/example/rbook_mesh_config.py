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

from common import (
    Options,
    Simulation,
)
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
    help="Enable FTR tracing with the given output basename",
)
parser.add_argument(
    "--trace-format",
    choices=("ftr", "text"),
    default="ftr",
    help="Trace output format when --trace-output is set",
)
parser.add_argument(
    "--wave-trace",
    action="store_true",
    help="Enable Ruby MessageBuffer FST waveform tracing (disabled by default)",
)
# ---------------------------------------------------------------------------
# Defaults below come from the hotspot-heavy single-thread bottleneck sweep
# (see ruby-book/final/hotspot-heavy/README.md "Investigation history"). The
# baseline stock-default config (seq=16 / L1D TBE=16 / LQ=32 / IQ=64 /
# HNF TBE=32) reaches 0.0558 line ops/cycle. The sweep identified
#
#   seq=32 / L1D TBE=32 / L2 TBE=64 / LQ=64 / IQ=128 / HNF TBE=64
#
# as the knee of throughput vs HNF15 queueing for the single-remote-HNF
# access pattern, reaching 0.0644 line ops/cycle (+15.3%). Going further
# (seq=64, LQ=128, IQ=256, L1D TBE=48) made HNF15 miss round-trip grow
# faster than concurrency gained and regressed the metric.
# ---------------------------------------------------------------------------

parser.add_argument(
    "--cpu-lq-entries",
    type=int,
    default=64,
    help="O3 CPU load queue entries (default 64; upstream default 32). "
    "Sized to hold the 32 in-flight misses allowed by L1D and the ~30 "
    "address-computation insts trailing them.",
)
parser.add_argument(
    "--cpu-sq-entries",
    type=int,
    default=None,
    help="Override the O3 CPU store queue entries",
)
parser.add_argument(
    "--cpu-rob-entries",
    type=int,
    default=None,
    help="Override the O3 CPU ROB entries",
)
parser.add_argument(
    "--cpu-iq-entries",
    type=int,
    default=128,
    help="O3 CPU instruction queue entries (default 128; upstream default "
    "64). IQ=64 was the secondary bind once the Ruby sequencer cap and L1D "
    "TBEs were widened.",
)
parser.add_argument(
    "--l1d-seq-outstanding",
    type=int,
    default=32,
    help="RubySequencer max_outstanding_requests on the L1D data sequencer "
    "(default 32; upstream default 16). This gate is the true MLP cap "
    "upstream of L1D TBEs - it caps how many demand requests the sequencer "
    "will accept concurrently, independent of LQ or TBE size.",
)
parser.add_argument(
    "--l1d-tbes",
    type=int,
    default=32,
    help="CHI L1D number_of_TBEs per CPU (default 32; upstream default 16). "
    "Matched to --l1d-seq-outstanding so TBEs never bottleneck upstream of "
    "the sequencer cap.",
)
parser.add_argument(
    "--l2-tbes",
    type=int,
    default=64,
    help="CHI private-L2 number_of_TBEs (default 64; upstream default 32). "
    "Raised to stay ahead of L1D under strict-inclusive pass-through.",
)
parser.add_argument(
    "--hnf-tbes",
    type=int,
    default=64,
    help="CHI HNF number_of_TBEs (default 64; upstream default 32). A "
    "single-remote-HNF hotspot saturates the home node's queue first; "
    "widening HNF TBEs was the step that flipped the sweep from regression "
    "to +9-15% improvement.",
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
    cpu_type="RiscvO3CPU",
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

CPUClass, mem_mode = Simulation.getCPUClass(args.cpu_type)

system = System(
    cpu=[CPUClass(cpu_id=i) for i in range(args.num_cpus)],
    mem_mode=mem_mode,
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

for cpu in system.cpu:
    if args.cpu_lq_entries is not None:
        cpu.LQEntries = args.cpu_lq_entries
    if args.cpu_sq_entries is not None:
        cpu.SQEntries = args.cpu_sq_entries
    if args.cpu_rob_entries is not None:
        cpu.numROBEntries = args.cpu_rob_entries
    if args.cpu_iq_entries is not None:
        for iq in cpu.instQueues:
            iq.numEntries = args.cpu_iq_entries

# --- Ruby / CHI / Garnet -----------------------------------------------------

Ruby.create_system(args, False, system)
assert args.num_cpus == len(system.ruby._cpu_ports)

for cpu in system.cpu:
    cpu.data_sequencer.max_outstanding_requests = args.l1d_seq_outstanding
    cpu.l1d.number_of_TBEs = args.l1d_tbes
    cpu.l1d.number_of_repl_TBEs = args.l1d_tbes
    cpu.l2.number_of_TBEs = args.l2_tbes
    cpu.l2.number_of_repl_TBEs = args.l2_tbes
for hnf in system.ruby.hnf:
    hnf.cntrl.number_of_TBEs = args.hnf_tbes
    hnf.cntrl.number_of_repl_TBEs = args.hnf_tbes

system.ruby.clk_domain = SrcClockDomain(
    clock=args.ruby_clock, voltage_domain=system.voltage_domain
)

for i in range(args.num_cpus):
    system.cpu[i].createInterruptController()
    system.ruby._cpu_ports[i].connectCpuPorts(system.cpu[i])

if args.trace_output is not None:
    system.ftr_trace = FtrTrace(
        output_format=args.trace_format, output_file=args.trace_output
    )

# --- Instantiate and run -----------------------------------------------------

root = Root(full_system=False, system=system)
if args.wave_trace:
    root.trace = FstTrace(trace_file="message_buffers.fst")
m5.instantiate()
exit_event = m5.simulate()

print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
