# Copyright (c) 2026 The gem5 Authors
# SPDX-License-Identifier: BSD-3-Clause

"""Two-core SCR1 RTL co-simulation tests for classic caches and Ruby."""

import sys

import testlib.log as log
from testlib import (
    UniqueFixture,
    config,
    constants,
    gem5_verify_config,
    verifier,
)
from testlib.helper import (
    joinpath,
    log_call,
)


class RtlCosimScr1Fixture(UniqueFixture):
    """Build the SCR1 vendor library and all gem5 validation images once."""

    def __new__(cls):
        build_dir = joinpath(config.build_dir, "rtl-cosim-scr1")
        target = joinpath(build_dir, "scr1", "librtl_cosim_scr1.so")
        return super().__new__(cls, target)

    def _init(self):
        self.name = "SCR1 RTL co-simulation artifacts"
        self.build_dir = joinpath(config.build_dir, "rtl-cosim-scr1")
        self.library = self.target
        self.program_dir = joinpath(self.build_dir, "scr1", "programs")

    def _setup(self, testitem):
        if config.skip_build:
            return

        configure = [
            "cmake",
            "-S",
            joinpath(config.base_dir, "src", "rtl"),
            "-B",
            self.build_dir,
            "-DRTL_COSIM_BUILD_TESTS=ON",
            "-DRTL_COSIM_BUILD_SCR1=ON",
            "-DRTL_COSIM_BUILD_PULP_FIXTURES=OFF",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DOBJCACHE_ENABLED=OFF",
        ]
        log_call(log.test_log, configure, time=None, stderr=sys.stderr)

        targets = ["rtl_cosim_scr1"] + [
            f"scr1_{name}_program" for name, _ in VALIDATIONS
        ]
        build = [
            "cmake",
            "--build",
            self.build_dir,
            "--parallel",
            str(config.threads),
            "--target",
            *targets,
        ]
        log_call(log.test_log, build, time=None, stderr=sys.stderr)


VALIDATIONS = (
    ("gem5_boot_memory", "idle"),
    ("gem5_deterministic_multicore", "idle"),
    ("gem5_dining_philosophers", "idle"),
    ("gem5_interrupt", "interrupt"),
    ("gem5_reset", "reset"),
    ("gem5_error", "error"),
)

fixture = RtlCosimScr1Fixture()
script = joinpath(
    config.base_dir, "configs", "example", "rtl_cosim", "scr1_two_core.py"
)

for memory_system in ("classic", "ruby"):
    protocol = "MESI_Two_Level" if memory_system == "ruby" else None
    for program, validation in VALIDATIONS:
        gem5_verify_config(
            name=f"rtl-cosim-scr1-{memory_system}-{program}",
            config=script,
            config_args=[
                "--library",
                fixture.library,
                "--image",
                joinpath(fixture.program_dir, f"{program}.elf"),
                "--memory-system",
                memory_system,
                "--validation",
                validation,
            ],
            verifiers=(
                verifier.MatchRegex(
                    rf"^RTL_COSIM_PASS memory_system={memory_system}$"
                ),
            ),
            fixtures=(fixture,),
            valid_isas=(constants.riscv_tag,),
            valid_variants=(constants.opt_tag,),
            valid_hosts=constants.supported_hosts,
            length=constants.long_tag,
            protocol=protocol,
        )
