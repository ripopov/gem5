# Copyright (c) 2006-2007 The Regents of The University of Michigan
# Copyright (c) 2009 Advanced Micro Devices, Inc.
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

import argparse
import os
import sys

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.util import addToPath

addToPath("../")

from common import Options
from ruby import Ruby

# Get paths we might need.  It's expected this file is in m5/configs/example.
config_path = os.path.dirname(os.path.abspath(__file__))
config_root = os.path.dirname(config_path)
m5_root = os.path.dirname(config_root)

parser = argparse.ArgumentParser()
Options.addNoISAOptions(parser)

parser.add_argument(
    "--maxloads", metavar="N", default=100, help="Stop after N loads"
)
parser.add_argument(
    "-f",
    "--wakeup_freq",
    metavar="N",
    default=10,
    help="Wakeup every N cycles",
)
parser.add_argument(
    "--check-flush",
    action="store_true",
    help="inject acknowledged FLUSH traffic while running the random checker",
)
parser.add_argument(
    "--flush-period",
    type=int,
    default=0,
    help="issue a deterministic FLUSH every N checker wakeups",
)
parser.add_argument(
    "--flush-duplicates",
    action="store_true",
    help="issue each FLUSH concurrently from two tester ports",
)

#
# Add the ruby specific and protocol specific options
#
Ruby.define_options(parser)

exec(
    compile(
        open(os.path.join(config_root, "common", "Options.py")).read(),
        os.path.join(config_root, "common", "Options.py"),
        "exec",
    )
)

args = parser.parse_args()

#
# Set the default cache size and associativity to be very small to encourage
# races between requests and writebacks.
#
args.l1d_size = "256B"
args.l1i_size = "256B"
args.l2_size = "512B"
args.l3_size = "1KiB"
args.l1d_assoc = 2
args.l1i_assoc = 2
args.l2_assoc = 2
args.l3_assoc = 2

#
# Create the ruby random tester
#

# Check the protocol
check_flush = False
if buildEnv["PROTOCOL"] == "MOESI_hammer":
    check_flush = True
if buildEnv["PROTOCOL"] == "MESI_Three_Level":
    check_flush = True
if args.flush_period < 0:
    parser.error("--flush-period must not be negative")
if args.flush_duplicates and args.num_cpus < 2:
    parser.error("--flush-duplicates requires at least two CPUs")
if args.check_flush or args.flush_period or args.flush_duplicates:
    check_flush = True

tester = RubyTester(
    check_flush=check_flush,
    flush_period=args.flush_period,
    flush_duplicates=args.flush_duplicates,
    checks_to_complete=args.maxloads,
    wakeup_frequency=args.wakeup_freq,
)

#
# Create the M5 system.  Note that the Memory Object isn't
# actually used by the rubytester, but is included to support the
# M5 memory size == Ruby memory size checks
#
system = System(cpu=tester, mem_ranges=[AddrRange(args.mem_size)])

# Create a top-level voltage domain and clock domain
system.voltage_domain = VoltageDomain(voltage=args.sys_voltage)

system.clk_domain = SrcClockDomain(
    clock=args.sys_clock, voltage_domain=system.voltage_domain
)

# The Ruby tester reuses num_cpus to specify its number of ports. CHI attaches
# split cache controllers and sequencers as children of each supplied CPU-like
# object, so reusing the tester object would overwrite those children. Give
# each CHI requester a distinct owner while retaining the one tester below.
if buildEnv["PROTOCOL"] == "CHI":
    system.chi_tester_nodes = [SubSystem() for _ in range(args.num_cpus)]
    cpu_list = system.chi_tester_nodes
else:
    cpu_list = [system.cpu] * args.num_cpus
Ruby.create_system(args, False, system, cpus=cpu_list)

# Create a seperate clock domain for Ruby
system.ruby.clk_domain = SrcClockDomain(
    clock=args.ruby_clock, voltage_domain=system.voltage_domain
)

assert args.num_cpus == len(system.ruby._cpu_ports)

tester.num_cpus = len(system.ruby._cpu_ports)

#
# The tester is most effective when randomization is turned on and
# artifical delay is randomly inserted on messages
#
system.ruby.randomization = True

for ruby_port in system.ruby._cpu_ports:
    #
    # Tie the ruby tester ports to the ruby cpu read and write ports
    #
    if ruby_port.support_data_reqs and ruby_port.support_inst_reqs:
        tester.cpuInstDataPort = ruby_port.in_ports
    elif ruby_port.support_data_reqs:
        tester.cpuDataPort = ruby_port.in_ports
    elif ruby_port.support_inst_reqs:
        tester.cpuInstPort = ruby_port.in_ports

    # Do not automatically retry stalled Ruby requests
    ruby_port.no_retry_on_stall = True

    #
    # Tell each sequencer this is the ruby tester so that it
    # copies the subblock back to the checker
    #
    ruby_port.using_ruby_tester = True

# -----------------------
# run simulation
# -----------------------

root = Root(full_system=False, system=system)
root.system.mem_mode = "timing"

# Not much point in this being higher than the L1 latency
m5.ticks.setGlobalFrequency("1ns")

# instantiate configuration
m5.instantiate()

# simulate until program terminates
exit_event = m5.simulate(args.abs_max_tick)

print("Exiting @ tick", m5.curTick(), "because", exit_event.getCause())
