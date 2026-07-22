# Copyright (c) 2026 The gem5 Authors
# SPDX-License-Identifier: BSD-3-Clause

"""gem5 validation for the SCR1 and PULP C910 RTL adapters."""

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


C910_SWITCH_CASES = (
    "integer",
    "compute",
    "fp",
    "trap",
    "sv39",
    "user",
    "atomic",
    "lrsc",
    "interrupt",
    "external_interrupt",
)


class RtlCosimC910Fixture(UniqueFixture):
    """Build the C910 vendor library and bare-metal programs once."""

    def __new__(cls):
        build_dir = joinpath(config.build_dir, "rtl-cosim-pulp-c910")
        target = joinpath(
            build_dir, "pulp-c910", "librtl_cosim_pulp_c910.so"
        )
        return super().__new__(cls, target)

    def _init(self):
        self.name = "PULP C910 RTL co-simulation artifacts"
        self.build_dir = joinpath(
            config.build_dir, "rtl-cosim-pulp-c910"
        )
        self.library = self.target
        self.program_dir = joinpath(
            self.build_dir, "pulp-c910", "programs"
        )

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
            "-DRTL_COSIM_BUILD_PULP_C910=ON",
            "-DRTL_COSIM_BUILD_PULP_FIXTURES=OFF",
            "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
            "-DOBJCACHE_ENABLED=OFF",
        ]
        log_call(log.test_log, configure, time=None, stderr=sys.stderr)

        targets = [
            "rtl_cosim_pulp_c910",
            "c910_gem5_benchmark_program",
            "c910_switch_programs",
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


c910_fixture = RtlCosimC910Fixture()
c910_script = joinpath(
    config.base_dir, "configs", "example", "rtl_cosim", "c910_single_core.py"
)
gem5_verify_config(
    name="rtl-cosim-pulp-c910-gem5-benchmark",
    config=c910_script,
    config_args=[
        "--library",
        c910_fixture.library,
        "--image",
        joinpath(c910_fixture.program_dir, "gem5_benchmark.elf"),
    ],
    verifiers=(verifier.MatchRegex(r"^RTL_COSIM_C910_PASS$"),),
    fixtures=(c910_fixture,),
    valid_isas=(constants.riscv_tag,),
    valid_variants=(constants.opt_tag,),
    valid_hosts=constants.supported_hosts,
    length=constants.long_tag,
)


c910_switch_script = joinpath(
    config.base_dir, "configs", "example", "rtl_cosim", "c910_cpu_switch.py"
)

for case in C910_SWITCH_CASES:
    gem5_verify_config(
        name=f"rtl-cosim-riscv-atomic-to-c910-{case}",
        config=c910_switch_script,
        config_args=[
            "--library",
            c910_fixture.library,
            "--image",
            joinpath(c910_fixture.program_dir, f"switch_{case}.elf"),
            "--case",
            case,
        ],
        verifiers=(
            verifier.MatchRegex(
                r"^RTL_COSIM_CPU_SWITCH_ONE_WAY_PASS$"
            ),
            verifier.MatchRegex(
                rf"^RTL_COSIM_CPU_SWITCH_PASS case={case}$"
            ),
        ),
        fixtures=(c910_fixture,),
        valid_isas=(constants.riscv_tag,),
        valid_variants=(constants.opt_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.long_tag,
    )

# Run the exact-state case twice to catch retained vendor-model or runtime
# state and prove deterministic reconstruction across independent processes.
for repetition in range(2):
    gem5_verify_config(
        name=f"rtl-cosim-riscv-atomic-to-c910-repeat-{repetition}",
        config=c910_switch_script,
        config_args=[
            "--library",
            c910_fixture.library,
            "--image",
            joinpath(c910_fixture.program_dir, "switch_integer.elf"),
            "--case",
            "integer",
        ],
        verifiers=(
            verifier.MatchRegex(
                r"^RTL_COSIM_CPU_SWITCH_ONE_WAY_PASS$"
            ),
            verifier.MatchRegex(
                r"^RTL_COSIM_CPU_SWITCH_PASS case=integer$"
            ),
        ),
        fixtures=(c910_fixture,),
        valid_isas=(constants.riscv_tag,),
        valid_variants=(constants.opt_tag,),
        valid_hosts=constants.supported_hosts,
        length=constants.long_tag,
    )
