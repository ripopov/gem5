"""
Minimal smoke test for FTR transaction tracing.
Usage:
  gem5.opt -d <outdir> ftr_smoke_test.py --protocol=MI_example \
    --num-cpus=2 --num-dirs=2 --network=garnet \
    --topology=Mesh_XY --mesh-rows=2
"""

import os
import sys

import m5
from m5.objects import *

sys.path.insert(
    0, os.path.join(os.path.dirname(__file__), "..", "..", "..", "configs")
)

import argparse

from common import Options
from ruby import Ruby

parser = argparse.ArgumentParser()
Options.addNoISAOptions(parser)
Ruby.define_options(parser)

exec(
    compile(
        open(
            os.path.join(
                os.path.dirname(__file__),
                "..",
                "..",
                "..",
                "configs",
                "common",
                "Options.py",
            )
        ).read(),
        "Options.py",
        "exec",
    )
)

args = parser.parse_args()

# Override CPU type for RISC-V build (Ruby.py defaults to X86).
# The tester doesn't use CPUs, but Ruby.create_system validates the type.
if not hasattr(args, "cpu_type") or "X86" in str(args.cpu_type):
    args.cpu_type = "RiscvTimingSimpleCPU"

args.l1d_size = "256B"
args.l1i_size = "256B"
args.l2_size = "512B"
args.l3_size = "1KiB"
args.l1d_assoc = 2
args.l1i_assoc = 2
args.l2_assoc = 2
args.l3_assoc = 2

tester = RubyTester(
    check_flush=False,
    checks_to_complete=50,
    wakeup_frequency=10,
)

system = System(cpu=tester, mem_ranges=[AddrRange(args.mem_size)])
system.voltage_domain = VoltageDomain(voltage=args.sys_voltage)
system.clk_domain = SrcClockDomain(
    clock=args.sys_clock, voltage_domain=system.voltage_domain
)

cpu_list = [system.cpu] * args.num_cpus
Ruby.create_system(args, False, system, cpus=cpu_list)

system.ruby.clk_domain = SrcClockDomain(
    clock=args.ruby_clock, voltage_domain=system.voltage_domain
)

tester.num_cpus = len(system.ruby._cpu_ports)
system.ruby.randomization = True

for ruby_port in system.ruby._cpu_ports:
    if ruby_port.support_data_reqs and ruby_port.support_inst_reqs:
        tester.cpuInstDataPort = ruby_port.in_ports
    elif ruby_port.support_data_reqs:
        tester.cpuDataPort = ruby_port.in_ports
    elif ruby_port.support_inst_reqs:
        tester.cpuInstPort = ruby_port.in_ports
    ruby_port.no_retry_on_stall = True
    ruby_port.using_ruby_tester = True

# Enable FTR tracing
system.ftr_trace = FtrTrace(output_format="text", output_file="transactions")

root = Root(full_system=False, system=system)
root.system.mem_mode = "timing"

m5.ticks.setGlobalFrequency("1ns")
m5.instantiate()

exit_event = m5.simulate(1000000)
print("Exiting @ tick", m5.curTick(), "because", exit_event.getCause())
