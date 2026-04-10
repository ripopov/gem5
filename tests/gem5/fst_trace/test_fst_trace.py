# Copyright (c) 2026 The Regents of the University of California
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

import subprocess

from testlib import *
from testlib.helper import joinpath

from gem5.fixture import SConsFixture


class FstTraceVerifierFixture(SConsFixture):
    def __new__(cls):
        target_dir = joinpath(config.build_dir, "RISCV")
        target = joinpath(
            target_dir, "sim", "fst_trace", "verify_fst_trace.opt"
        )
        obj = super().__new__(cls, target)
        obj.target_dir = target_dir
        return obj

    def _init(self):
        self.name = "fst-trace-verifier"
        self.targets = [self.target]
        self.path = self.target
        self.directory = config.base_dir
        self.isa = "riscv"
        self.protocol = None
        self.set_global()


class VerifyFstTrace(verifier.Verifier):
    def __init__(self, verify_fixture, scenario, trace_name):
        super().__init__(fixtures=(verify_fixture,))
        self._verify_fixture = verify_fixture
        self._scenario = scenario
        self._trace_name = trace_name

    def test(self, params):
        tempdir = params.fixtures[constants.tempdir_fixture_name].path
        trace_path = joinpath(tempdir, self._trace_name)
        result = subprocess.run(
            [self._verify_fixture.path, self._scenario, trace_path],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise Exception(
                "FST verifier failed.\n"
                f"stdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}"
            )


fst_trace_verifier = FstTraceVerifierFixture()

gem5_verify_config(
    name="fst-trace-events",
    fixtures=(fst_trace_verifier,),
    verifiers=(
        VerifyFstTrace(fst_trace_verifier, "events", "events.fst"),
        verifier.MatchRegex(r"Goodbye hello!!"),
    ),
    config=joinpath(
        config.base_dir,
        "tests",
        "gem5",
        "fst_trace",
        "configs",
        "fst_events.py",
    ),
    config_args=[],
    valid_isas=(constants.riscv_tag,),
    valid_hosts=constants.supported_hosts,
    length=constants.quick_tag,
)

gem5_verify_config(
    name="fst-trace-memory-traffic",
    fixtures=(fst_trace_verifier,),
    verifiers=(
        VerifyFstTrace(fst_trace_verifier, "traffic", "traffic.fst"),
        verifier.MatchRegex(r".*encountered the exit state"),
    ),
    config=joinpath(
        config.base_dir,
        "tests",
        "gem5",
        "fst_trace",
        "configs",
        "fst_memory_traffic.py",
    ),
    config_args=[],
    valid_isas=(constants.riscv_tag,),
    valid_hosts=constants.supported_hosts,
    length=constants.quick_tag,
)

gem5_verify_config(
    name="fst-trace-stat-sampling",
    fixtures=(fst_trace_verifier,),
    verifiers=(
        VerifyFstTrace(fst_trace_verifier, "stats", "stats.fst"),
        verifier.MatchRegex(r".*encountered the exit state"),
    ),
    config=joinpath(
        config.base_dir,
        "tests",
        "gem5",
        "fst_trace",
        "configs",
        "fst_stats.py",
    ),
    config_args=[],
    valid_isas=(constants.riscv_tag,),
    valid_hosts=constants.supported_hosts,
    length=constants.quick_tag,
)
