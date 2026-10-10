# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
"""Bare-metal reduction and pipelined DMA regression coverage."""

import os
import shutil
import struct
import subprocess
from pathlib import Path

from testlib import (
    config,
    constants,
    gem5_verify_config,
)

from gem5.fixture import UniqueFixture
from gem5.verifier import Verifier

ROOT = Path(config.base_dir)
PICOLIBC = Path(
    os.environ.get("PICOLIBC", "/usr/lib/picolibc/riscv64-unknown-elf")
)
HAVE_FIRMWARE_DEPS = (
    shutil.which("riscv64-unknown-elf-gcc") and PICOLIBC.is_dir()
)


class AcceleratorFirmware(UniqueFixture):
    def _init(self, target):
        self.name = "rt-accelerator-firmware"

    def _setup(self, testitem):
        subprocess.run(
            ["make", "-C", str(ROOT / "util/rt_reference"), "reduction"],
            check=True,
        )


class ReductionResult(Verifier):
    def test(self, params):
        output = Path(params.fixtures[constants.tempdir_fixture_name].path)
        partials = struct.unpack(
            "<16f", (output / "partials.bin").read_bytes()
        )
        assert partials == (262144.0,) * 16
        assert struct.unpack("<f", (output / "sum.bin").read_bytes()) == (
            4194304.0,
        )


firmware = AcceleratorFirmware(
    str(ROOT / "util/rt_reference/out/reduction-accelerator.elf")
)
for workload, queues, length, controllers in (
    ("reduction", 1, constants.long_tag, 1),
    ("reduction", 17, constants.long_tag, 1),
    ("reduction", 1, constants.long_tag, 2),
    ("reduction", 17, constants.long_tag, 2),
    ("reduction", 1, constants.long_tag, 4),
    ("reduction", 17, constants.long_tag, 4),
    ("dma-test", 1, constants.quick_tag, 1),
    ("dma-test", 2, constants.quick_tag, 1),
    ("dma-fault", 1, constants.quick_tag, 1),
    ("dma-fault", 2, constants.quick_tag, 1),
):
    # These bare-metal suites require an optional cross-toolchain. Avoid
    # registering them when it is absent; other TestLib suites still run.
    if not HAVE_FIRMWARE_DEPS:
        continue
    arguments = [f"--workload={workload}", f"--queues={queues}"]
    # Exercise the four-controller default without an explicit override.
    if controllers != 4:
        arguments.append(f"--memory-controllers={controllers}")
    if workload in ("dma-test", "dma-fault"):
        arguments += ["--queue-depth=2", "--dma-outstanding=16"]
    gem5_verify_config(
        name=(
            f"rt-accel-{workload}-q{queues}"
            + (f"-mc{controllers}" if controllers != 1 else "")
        ),
        config=str(ROOT / "configs/example/rt_accel_reduction.py"),
        config_args=arguments,
        fixtures=(firmware,),
        verifiers=(ReductionResult(),) if workload == "reduction" else (),
        valid_isas=(constants.riscv_tag,),
        valid_variants=(constants.opt_tag,),
        valid_hosts=(constants.host_x86_64_tag,),
        length=length,
    )
