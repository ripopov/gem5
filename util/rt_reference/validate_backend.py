# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
"""Validate and time identical configurable-core gem5 runs, sequentially."""

import argparse
import hashlib
import json
import os
import platform
import re
import statistics
import struct
import subprocess
import time
from pathlib import Path

from experiment import (
    compare_statistics,
    measured_run,
    require_idle,
    simulation_processes,
    stats,
    stats_blocks,
    symbols,
)
from PIL import Image
from validate import (
    analytic_smoke,
    scene_data,
)

ROOT = Path(__file__).resolve().parents[2]
GOLDEN = "79aaad3486ca49955cda1dd8fd6f36b8102cbbd3c89117f327b0c917c090ccaa"


def wire_evidence(directory, jobs, job_base, scene_bytes, rgba):
    cores = []
    msi_ticks = []
    for index, job in enumerate(jobs):
        scene_addr, output_addr, stride, x, y, width, height, *_ = job
        output_bytes = {
            output_addr + (y + row) * stride + 4 * x + byte
            for row in range(height)
            for byte in range(4 * width)
        }
        descriptor_bytes = set(range(scene_addr, scene_addr + scene_bytes))
        descriptor_bytes.update(
            range(job_base + index * 64, job_base + index * 64 + 56)
        )
        written, read = set(), set()
        output_responses = []
        msi = []
        lines = (directory / f"accel{index}.trace").read_text().splitlines()
        for line in lines:
            tick, stage, cmd, address, size, data = line.split()
            tick, address, size = int(tick), int(address, 16), int(size)
            extent = set(range(address, address + size))
            if stage == "master_accept" and cmd == "ReadReq":
                assert extent <= descriptor_bytes and not extent & read
                read.update(extent)
            if stage == "master_accept" and cmd == "WriteReq":
                if address == 0x20000000 + index * 4:
                    assert size == 4 and bytes.fromhex(data) == struct.pack(
                        "<I", index + 1
                    )
                    msi.append(tick)
                else:
                    assert extent <= output_bytes and not extent & written
                    written.update(extent)
                    offset = address - output_addr
                    assert bytes.fromhex(data) == rgba[offset : offset + size]
            if (
                stage == "master_arrival"
                and cmd == "WriteResp"
                and address in output_bytes
            ):
                output_responses.append(tick)
        assert read == descriptor_bytes
        assert written == output_bytes
        assert len(msi) == 1 and output_responses
        assert msi[0] > max(output_responses)
        host_lines = (
            (directory / f"host{index}.trace").read_text().splitlines()
        )
        terminal_status = [
            int(line.split()[0])
            for line in host_lines
            if "mmio_accept ReadReq " + f"{0x10000014 + index * 0x1000:x} "
            in line
        ]
        assert len(terminal_status) == 1
        visible = [
            int(line.split()[0])
            for line in (directory / "msi.trace").read_text().splitlines()
            if "msi_access WriteReq " + f"{0x20000000 + index * 4:x} " in line
        ]
        assert len(visible) == 1 and terminal_status[0] > visible[0]
        msi_ticks.extend(visible)
        cores.append(
            {
                "descriptor_read_bytes": len(read),
                "output_write_bytes": len(written),
                "last_output_response_tick": max(output_responses),
                "msi_issue_tick": msi[0],
                "msi_visible_tick": visible[0],
                "status_read_tick": terminal_status[0],
            }
        )
    assert all(core["status_read_tick"] > max(msi_ticks) for core in cores)
    return cores


def check_accelerators(system, queues, accelerator_cpu, accelerator_cores):
    assert len(system["accelerators"]) == accelerator_cores
    cpu_type = (
        "BaseO3CPU" if accelerator_cpu == "o3" else "BaseTimingSimpleCPU"
    )
    for index, accel in enumerate(system["accelerators"]):
        queue = index + 1 if queues != 1 else 0
        assert accel["eventq_index"] == queue
        assert accel["cpu"]["type"] == cpu_type
        assert accel["cpu"]["numThreads"] == 1
        assert accel["clk_domain"]["clock"] == [1000]
        assert accel["cpu"]["isa"][0]["riscv_profile"] == "RVA20S64"
        assert accel["cpu"]["isa"][0]["extra_extensions"] == []
        assert "V" not in accel["cpu"]["isa"][0]["reported_extensions"]
        for name in (
            "cpu",
            "control",
            "code_tcm",
            "data_tcm",
            "bus",
            "wrapper",
            "router_clock",
        ):
            assert accel[name]["eventq_index"] == queue
        assert accel["bus"]["type"] == "CoherentXBar"
        assert accel["control"]["coherent_dma"] is True
        for name, size, ways, mshrs in (
            ("icache", 8192, 2, 4),
            ("dcache", 16384, 4, 16),
        ):
            cache = accel[name]
            assert cache["eventq_index"] == queue
            assert cache["clk_domain"] == accel["clk_domain"]["path"]
            assert cache["size"] == size and cache["assoc"] == ways
            assert cache["mshrs"] == mshrs
        assert accel["wrapper"]["type"] == "RTGem5Accelerator"
        assert accel["control"]["type"] == "RTAccelControl"
        assert accel["code_tcm"]["type"] == "RTAccelCodeTCM"
        assert accel["code_tcm"]["range"] == "1073741824:1073807360"
        assert accel["code_tcm"]["writeable"] is False
        assert accel["code_tcm"]["latency"] == 1000
        assert accel["data_tcm"]["range"] == "1073807360:1074003968"
        assert accel["data_tcm"]["latency"] == 1000
        assert accel["data_tcm"]["bandwidth"] == "7.000000"
        assert accel["cpu"]["icache_port"]["peer"] == (
            accel["icache"]["path"] + ".cpu_side"
        )
        assert accel["icache"]["mem_side"]["peer"] == (
            accel["code_tcm"]["path"] + ".instruction"
        )
        assert accel["cpu"]["dcache_port"]["peer"] == (
            accel["dcache"]["path"] + ".cpu_side"
        )
        assert accel["dcache"]["mem_side"]["peer"].startswith(
            accel["bus"]["path"] + ".cpu_side_ports["
        )
        assert accel["code_tcm"]["constants"]["peer"].startswith(
            accel["bus"]["path"] + ".mem_side_ports["
        )
        assert accel["control"]["data_tcm_range"] == (
            accel["data_tcm"]["range"]
        )
        assert accel["memories"] == [
            accel["code_tcm"]["path"],
            accel["data_tcm"]["path"],
        ]
        assert accel["router_clock"]["clock"] == [250]
        assert accel["bus"]["width"] == 64
        for field in (
            "frontend_latency",
            "forward_latency",
            "response_latency",
        ):
            assert accel["bus"][field] == 0
        assert system["adapters"][index]["eventq_index"] == 0
        base = 0x10000000 + 0x1000 * index
        assert system["adapters"][index]["mmio_range"] == (
            f"{base}:{base + 0x1000}"
        )


def check_memory(system, controllers):
    memories = [
        system["memory" if i == 0 else f"memory{i}"]
        for i in range(controllers)
    ]
    assert system["memories"] == [memory["path"] for memory in memories]
    for i, memory in enumerate(memories):
        assert memory["type"] == "SimpleMemory"
        assert memory["eventq_index"] == 0
        assert memory["clk_domain"] == "system.clk_domain"
        assert memory["latency"] == 30000
        assert memory["bandwidth"] == "73.000000"
        expected_range = f"{0x80000000}:{0x84000000}"
        if controllers > 1:
            masks = ":".join(
                str(1 << (6 + bit))
                for bit in range(controllers.bit_length() - 1)
            )
            expected_range += f":{i}:{masks}"
        assert memory["range"] == expected_range
        assert memory["port"]["peer"].startswith(
            "system.membus.mem_side_ports["
        )
    return memories


def check_transport(config, transport_delay):
    quantum = config["sim_quantum"]
    assert quantum > 0
    if transport_delay == "5ns":
        assert quantum == 5000
    for accel in config["system"]["accelerators"]:
        wrapper = accel["wrapper"]
        assert wrapper["request_delay"] == quantum
        assert wrapper["response_delay"] == quantum
    return quantum


def inspect(directory, queues, accelerator_cpu="timing", accelerator_cores=8):
    log = (directory / "run.log").read_text()
    assert re.search(r"Stopped at \d+: m5_exit instruction encountered", log)
    config = json.loads((directory / "config.json").read_text())
    system = config["system"]
    assert system["eventq_index"] == 0
    assert system["cpu"]["type"] == "BaseO3CPU"
    assert system["cpu"]["numThreads"] == 1
    assert system["clk_domain"]["clock"] == [1000]
    assert system["host_cache"]["type"] == "Cache"
    assert system["membus"]["type"] == "NoncoherentXBar"
    assert len(system["accelerators"]) == accelerator_cores
    assert len(system["adapters"]) == accelerator_cores
    assert system["msi"]["accelerator_cores"] == accelerator_cores
    assert struct.unpack(
        "<I", (directory / "core_count.bin").read_bytes()
    ) == (accelerator_cores,)
    parameters = json.loads((directory / "parameters.json").read_text())
    quantum = check_transport(config, parameters["transport_delay"])
    controllers = parameters.get("memory_controllers", 1)
    memories = check_memory(system, controllers)
    assert parameters.get("accelerator_cpu", "timing") == accelerator_cpu
    assert parameters.get("accelerator_cores", 8) == accelerator_cores
    kernel_symbols = symbols(ROOT / "util/rt_reference/out/accelerator.elf")
    code_bytes = kernel_symbols["__code_end"] - 0x40000000
    data_bytes = kernel_symbols["__stack_top"] - 0x40010000
    assert 0 < code_bytes <= 65536
    assert 0 < data_bytes <= 196608
    tcm_bytes = code_bytes + data_bytes
    check_accelerators(system, queues, accelerator_cpu, accelerator_cores)
    scene = (directory / "scene.bin").read_bytes()
    scene_model = scene_data(scene)
    host_symbols = symbols(ROOT / "util/rt_reference/out/host-gem5.elf")
    raw_jobs = []
    decoded_jobs = []
    covered = set()
    for index in range(accelerator_cores):
        raw = (directory / f"job{index}.bin").read_bytes()
        job = struct.unpack("<QQ8IQ", raw)
        assert job[:2] == (host_symbols["scene_bytes"], host_symbols["pixels"])
        stride, x, y, width, height = job[2:7]
        assert stride == 384 and width > 0 and height > 0
        assert x + width <= 96 and y + height <= 96
        assigned = {
            (col, row)
            for row in range(y, y + height)
            for col in range(x, x + width)
        }
        assert not assigned & covered
        covered.update(assigned)
        assert job[7:] == (1, 1, 0, 1)
        raw_jobs.append(raw)
        decoded_jobs.append(job)
    assert len(covered) == 96 * 96
    rgba = (directory / "image.rgba").read_bytes()
    assert len(rgba) == 96 * 96 * 4 and set(rgba[3::4]) == {255}
    sha = hashlib.sha256(rgba).hexdigest()
    assert sha == GOLDEN, f"smoke golden mismatch: {sha}"
    bmp = Image.open(directory / "image.bmp").convert("RGB")
    assert bmp.size == (96, 96)
    assert bmp.tobytes() == bytes(v for i, v in enumerate(rgba) if i % 4 != 3)
    bmp.save(directory / "image.png")
    table = []
    for byte in range(256):
        linear = byte / 255
        display = (
            12.92 * linear
            if linear <= 0.0031308
            else 1.055 * linear ** (1 / 2.4) - 0.055
        )
        table.append(int(255 * display + 0.5))
    bmp.point(table * 3).save(directory / "preview.png")
    error = analytic_smoke(scene_model, rgba)
    blocks = stats_blocks(directory)
    values = blocks[0]
    if len(blocks) > 1:
        assert len(blocks) == 2, "Expected one batch and one exit dump"
        assert 0 < values["simTicks"] < values["finalTick"]
        assert values["finalTick"] < blocks[-1]["finalTick"]
        for name, value in blocks[-1].items():
            if "accelerators" in name and name.endswith(
                (".numCycles", ".commitStats0.numInsts")
            ):
                assert value == 0, f"Accelerator ran after batch dump: {name}"
    # Local accelerator requestor IDs must be translated at the host boundary.
    # Each adapter reads one job plus the scene, and writes exactly its tile.
    for index in range(accelerator_cores):
        adapter = system["adapters"][index]["name"]
        assert sum(
            values.get(f"{memory['path']}.bytesRead::{adapter}", 0)
            for memory in memories
        ) == (56 + len(scene))
        assert sum(
            values.get(f"{memory['path']}.bytesWritten::{adapter}", 0)
            for memory in memories
        ) == (decoded_jobs[index][5] * decoded_jobs[index][6] * 4)
        # Statistics vector groups use numeric indices without the padding
        # used in SimObject paths once the count reaches ten.
        prefix = "system.accelerators" + (
            str(index) if accelerator_cores > 1 else ""
        )
        code = f"{prefix}.code_tcm"
        data = f"{prefix}.data_tcm"
        assert values[f"{code}.bytesInstRead::cpu.inst"] > 0
        assert values[f"{code}.bytesRead::cpu.data"] > 0
        assert values.get(f"{data}.bytesInstRead::total", 0) == 0
        assert values.get(f"{code}.bytesWritten::total", 0) == 0
        assert values[f"{data}.bytesWritten::control"] > 0
    clocks = {
        name: value
        for name, value in values.items()
        if name.endswith(".numCycles")
    }
    instructions = {
        name: value
        for name, value in values.items()
        if name.endswith(".numInsts")
    }
    assert len(clocks) >= accelerator_cores + 1
    wire = (
        wire_evidence(
            directory, decoded_jobs, host_symbols["jobs"], len(scene), rgba
        )
        if (directory / "accel0.trace").is_file()
        else None
    )
    evidence = {
        "quantum_ticks": quantum,
        "transport_delay": parameters["transport_delay"],
        "wire": wire,
        "queues": queues,
        "accelerator_cpu": accelerator_cpu,
        "accelerator_caches": parameters["accelerator_caches"],
        "memory_controllers": controllers,
        "controller_statistics": [
            {
                "path": memory["path"],
                "read_bytes": int(
                    values[f"{memory['path']}.bytesRead::total"]
                ),
                "write_bytes": int(
                    values[f"{memory['path']}.bytesWritten::total"]
                ),
            }
            for memory in memories
        ],
        "sha256": sha,
        "simTicks": values["simTicks"],
        "statistics_dumps": len(blocks),
        "stats_region_start_tick": values["finalTick"] - values["simTicks"],
        "stats_region_end_tick": values["finalTick"],
        "simulation_final_tick": blocks[-1]["finalTick"],
        "simulation_clock_periods": blocks[-1]["finalTick"]
        / system["clk_domain"]["clock"][0],
        "elapsed_clock_periods": values["simTicks"]
        / system["clk_domain"]["clock"][0],
        "clock_cycles": clocks,
        "instructions": instructions,
        "host_seconds": values["hostSeconds"],
        "tcm_layout_bytes": tcm_bytes,
        "code_tcm_layout_bytes": code_bytes,
        "data_tcm_layout_bytes": data_bytes,
        "analytic_max_byte_error": error,
        "tiles": accelerator_cores,
        "accelerator_cores": accelerator_cores,
    }
    if accelerator_cpu == "o3":
        cores = []
        for index in range(accelerator_cores):
            prefix = "system.accelerators" + (
                str(index) if accelerator_cores > 1 else ""
            )
            retired = int(values[f"{prefix}.cpu.commitStats0.numInsts"])
            cycles = int(values[f"{prefix}.cpu.numCycles"])
            cache_statistics = {}
            for cache in ("icache", "dcache"):
                for counter in (
                    "overallHits::total",
                    "overallMisses::total",
                    "overallMshrHits::total",
                    "overallMshrMisses::total",
                ):
                    name = f"{cache}.{counter}"
                    # Zero-valued MSHR vectors can be omitted by gem5.
                    cache_statistics[name] = int(
                        values.get(f"{prefix}.{name}", 0)
                    )
                assert (
                    cache_statistics[f"{cache}.overallHits::total"]
                    + cache_statistics[f"{cache}.overallMisses::total"]
                    > 0
                )
            cores.append(
                {
                    "core": index,
                    "retired_instructions": retired,
                    "counted_cycles": cycles,
                    "ipc": retired / cycles,
                    "cache_statistics": cache_statistics,
                    "dma": {
                        counter: values[f"{prefix}.control.{counter}"]
                        for counter in (
                            "bytesCopied",
                            "busyTicks",
                            "readRequests",
                            "writeRequests",
                            "cleanRequests",
                            "readRetries",
                            "writeRetries",
                            "peakChunks",
                        )
                    },
                }
            )
        retired = sum(core["retired_instructions"] for core in cores)
        cycles = sum(core["counted_cycles"] for core in cores)
        evidence["accelerator_ipc"] = {
            "retired_instructions": retired,
            "counted_cycles": cycles,
            "cycle_weighted_core_ipc": retired / cycles,
            "combined_cluster_ipc": retired
            / evidence["elapsed_clock_periods"],
            "per_core": cores,
        }
    (directory / "validation.json").write_text(
        json.dumps(evidence, indent=2) + "\n"
    )
    return evidence


def compare(left, right, traces=True):
    left_parameters = json.loads((left / "parameters.json").read_text())
    right_parameters = json.loads((right / "parameters.json").read_text())
    cores = left_parameters.get("accelerator_cores", 8)
    assert cores == right_parameters.get("accelerator_cores", 8)
    for name in (
        "scene.bin",
        "job.bin",
        "image.rgba",
        "image.bmp",
        "parameters.bin",
        "core_count.bin",
        *(f"job{i}.bin" for i in range(cores)),
    ):
        assert (left / name).read_bytes() == (right / name).read_bytes(), name
    comparison = compare_statistics(left, right)
    trace_hashes = {}
    if traces:
        for name in (
            "msi.trace",
            *(f"host{i}.trace" for i in range(cores)),
            *(f"accel{i}.trace" for i in range(cores)),
        ):
            data = (left / name).read_bytes()
            assert data and data == (right / name).read_bytes(), name
            trace_hashes[name] = hashlib.sha256(data).hexdigest()
    return {
        **comparison,
        "transaction_trace_sha256": trace_hashes,
    }


def run_protocol(args, reuse=False):
    native = args.output / "protocol-native"
    native.mkdir(exist_ok=True)
    subprocess.run(
        [str(ROOT / "util/rt_reference/out/native"), "1", "1", "0", "1"],
        cwd=native,
        check=True,
    )
    reference = (native / "image.rgba").read_bytes()
    assert hashlib.sha256(reference).hexdigest() == GOLDEN
    for queues in (1, args.accelerator_cores + 1):
        directory = args.output / f"protocol-q{queues}"
        directory.mkdir(exist_ok=True)
        command = [
            str(args.gem5.resolve()),
            f"--outdir={directory}",
            str(ROOT / "configs/example/rt_accel.py"),
            "--backend=gem5",
            f"--accelerator-cpu={args.accelerator_cpu}",
            f"--accelerator-cores={args.accelerator_cores}",
            f"--queues={queues}",
            "--render-mode=1",
            "--queue-depth=1",
            "--trace",
            f"--transport-delay={args.transport_delay}",
            "--host-firmware="
            + str(ROOT / "util/rt_reference/out/host-test.elf"),
            "--accelerator-firmware="
            + str(ROOT / "util/rt_reference/out/accelerator-test.elf"),
        ]
        if args.memory_controllers != 4:
            command.append(f"--memory-controllers={args.memory_controllers}")
        if not args.inspect_only and not reuse:
            print("Protocol:", " ".join(command), flush=True)
            with (directory / "run.log").open("w") as log:
                subprocess.run(
                    command,
                    cwd=ROOT,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    check=True,
                )
        assert (
            "m5_exit instruction encountered"
            in (directory / "run.log").read_text()
        )
        blocks = stats_blocks(directory)
        assert len(blocks) == 5, "Expected four batches and one exit dump"
        for block in blocks[:-1]:
            assert 0 < block["simTicks"] < block["finalTick"]
        for name, value in blocks[-1].items():
            if "accelerators" in name and name.endswith(".numCycles"):
                assert value == 0, f"Accelerator ran during export: {name}"
        config = json.loads((directory / "config.json").read_text())
        check_transport(config, args.transport_delay)
        check_memory(config["system"], args.memory_controllers)
        cpu_type = (
            "BaseO3CPU"
            if args.accelerator_cpu == "o3"
            else "BaseTimingSimpleCPU"
        )
        assert all(
            accel["cpu"]["type"] == cpu_type
            for accel in config["system"]["accelerators"]
        )
        check_accelerators(
            config["system"],
            queues,
            args.accelerator_cpu,
            args.accelerator_cores,
        )
        assert struct.unpack(
            "<I", (directory / "core_count.bin").read_bytes()
        ) == (args.accelerator_cores,)
        expected = bytearray(96 * 96 * 4)
        saved = bytearray()
        for i in range(args.accelerator_cores):
            job = struct.unpack(
                "<QQ8IQ", (directory / f"job{i}.bin").read_bytes()
            )
            assert job[2] == 384 and job[5:7] == (1, 1)
            offset = 4 * (job[3] + 96 * job[4])
            expected[offset : offset + 4] = reference[offset : offset + 4]
            saved.extend(reference[offset : offset + 4])
        assert (directory / "image.rgba").read_bytes() == expected
        assert (directory / "protocol.bin").read_bytes() == saved
        msi = (directory / "msi.trace").read_text()
        count_data = struct.pack("<I", args.accelerator_cores).hex()
        assert f"msi_result ReadResp 20000108 4 {count_data}" in msi
        rejects = 0
        for i in range(args.accelerator_cores):
            host = (directory / f"host{i}.trace").read_text()
            accel = (directory / f"accel{i}.trace").read_text()
            assert host.count("master_arrival WriteReq 200000") == 4
            assert (
                host.count(
                    "mmio_accept WriteReq " + f"{0x10000000 + 0x1000 * i:x} 8 "
                )
                == 4
            )
            submissions = re.findall(
                rf"^(\d+) mmio_accept WriteReq {0x10000000 + 0x1000 * i:x} 8 ",
                host,
                re.MULTILINE,
            )
            for tick, block in zip(submissions, blocks[:-1]):
                assert (
                    block["finalTick"] - block["simTicks"]
                    < int(tick)
                    < block["finalTick"]
                ), "Submission outside its reset/dump interval"
            rejects += accel.count("master_reject")
        assert rejects, "protocol test never exercised master backpressure"
    result = compare(
        args.output / "protocol-q1",
        args.output / f"protocol-q{args.accelerator_cores + 1}",
    )
    result["batch_statistics_dumps"] = 4
    result["export_accelerator_cycles"] = 0
    (args.output / "protocol-validation.json").write_text(
        json.dumps(result, indent=2) + "\n"
    )
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--gem5", type=Path, default=ROOT / "build/RISCV/gem5.opt"
    )
    parser.add_argument(
        "--output", type=Path, default=ROOT / "m5out/rt_backend/validated"
    )
    parser.add_argument("--runs", type=int, default=4)
    parser.add_argument("--accelerator-cores", type=int, default=8)
    parser.add_argument(
        "--memory-controllers", type=int, choices=(1, 2, 4), default=4
    )
    parser.add_argument(
        "--timing-only",
        action="store_true",
        help="Run every smoke pair without traces and include it in timing",
    )
    parser.add_argument(
        "--protocol-only",
        action="store_true",
        help="Run only the small traced protocol workload",
    )
    parser.add_argument(
        "--accelerator-cpu", choices=("timing", "o3"), default="timing"
    )
    parser.add_argument("--transport-delay", default="5ns")
    parser.add_argument("--inspect-only", action="store_true")
    parser.add_argument(
        "--reuse-correctness",
        action="store_true",
        help="Reuse the completed protocol and traced pair after checking "
        "the executable, firmware, CPU model and timing parameters; "
        "run new timing pairs",
    )
    parser.add_argument(
        "--require-idle-machine",
        action="store_true",
        help="Reject competing gem5 processes before/during timing runs; "
        "run in the host PID namespace so ps can see other simulations",
    )
    args = parser.parse_args()
    assert args.runs >= 1
    assert 1 <= args.accelerator_cores <= 32
    queue_modes = (1, args.accelerator_cores + 1)
    if args.timing_only and args.reuse_correctness:
        parser.error("timing-only cannot reuse a traced correctness pair")
    if args.require_idle_machine and not args.timing_only:
        assert args.runs >= 2, "Use at least one untraced timing sample"
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.require_idle_machine and not args.inspect_only:
        require_idle(simulation_processes())
    gem5_sha = hashlib.sha256(args.gem5.read_bytes()).hexdigest()
    firmware_sha = {
        name: hashlib.sha256(
            (ROOT / "util/rt_reference/out" / name).read_bytes()
        ).hexdigest()
        for name in (
            "host-gem5.elf",
            "accelerator.elf",
            "host-test.elf",
            "accelerator-test.elf",
        )
    }
    source_sha = {
        name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
        for name in (
            "configs/example/rt_accel.py",
            "configs/example/rt_accel_system.py",
            "src/dev/rt_accel/control.cc",
            "src/dev/rt_accel/control.hh",
            "src/dev/rt_accel/code_tcm.cc",
            "src/dev/rt_accel/code_tcm.hh",
            "src/dev/rt_accel/RTAccelerator.py",
            "util/rt_reference/host.c",
            "util/rt_reference/host_runtime.c",
            "util/rt_reference/host_runtime.h",
            "util/rt_reference/accelerator.c",
            "util/rt_reference/accelerator_runtime.c",
            "util/rt_reference/accelerator_runtime.h",
            "util/rt_reference/renderer.c",
            "util/rt_reference/renderer.h",
            "util/rt_reference/start.S",
            "util/rt_reference/link.ld",
            "util/rt_reference/accelerator.ld",
            "util/rt_reference/Makefile",
            "util/rt_reference/experiment.py",
            "util/rt_reference/validate.py",
            "util/rt_reference/validate_backend.py",
        )
    }
    if args.reuse_correctness:
        saved = json.loads((args.output / "validation.json").read_text())
        assert saved["gem5_sha256"] == gem5_sha
        assert saved["firmware_sha256"] == firmware_sha
        assert saved["quantum"] == args.transport_delay
        assert saved["transport_delay"] == args.transport_delay
        assert saved.get("accelerator_cpu", "timing") == args.accelerator_cpu
        assert saved.get("accelerator_cores", 8) == args.accelerator_cores
        assert saved.get("memory_controllers", 1) == args.memory_controllers
    protocol = run_protocol(args, reuse=args.reuse_correctness)
    if args.protocol_only:
        print(f"PASS: {args.accelerator_cores}-core protocol")
        return
    runs = []
    for repeat in range(args.runs):
        # Alternate order to reduce temperature/order bias. Never run two
        # competing simulations in this measurement harness.
        for queues in (queue_modes if repeat % 2 == 0 else queue_modes[::-1]):
            directory = args.output / f"q{queues}-{repeat}"
            directory.mkdir(exist_ok=True)
            command = [
                str(args.gem5.resolve()),
                f"--outdir={directory}",
                str(ROOT / "configs/example/rt_accel.py"),
                "--backend=gem5",
                f"--accelerator-cpu={args.accelerator_cpu}",
                f"--accelerator-cores={args.accelerator_cores}",
                f"--queues={queues}",
                "--render-mode=1",
                "--samples-per-pixel=1",
                "--max-secondary-depth=0",
                "--seed=1",
                f"--transport-delay={args.transport_delay}",
            ]
            if args.memory_controllers != 4:
                command.append(
                    f"--memory-controllers={args.memory_controllers}"
                )
            if repeat == 0 and not args.timing_only:
                command.append("--trace")
            if not args.inspect_only and not (
                repeat == 0 and args.reuse_correctness
            ):
                print("Running:", " ".join(command), flush=True)
                measured_run(
                    command,
                    directory,
                    require_idle_machine=args.require_idle_machine,
                )
            result = inspect(
                directory, queues, args.accelerator_cpu, args.accelerator_cores
            )
            parameters = json.loads(
                (directory / "parameters.json").read_text()
            )
            assert parameters["transport_delay"] == args.transport_delay
            assert result["memory_controllers"] == args.memory_controllers
            result.update(json.loads((directory / "wall.json").read_text()))
            result["simulated_clock_hz"] = (
                result["simulation_clock_periods"] / result["wall_seconds"]
            )
            result["repeat"] = repeat
            runs.append(result)
        compare(
            args.output / f"q1-{repeat}",
            args.output / f"q{queue_modes[1]}-{repeat}",
            traces=repeat == 0 and not args.timing_only,
        )
        if repeat:
            compare(
                args.output / "q1-0",
                args.output / f"q1-{repeat}",
                traces=False,
            )
    timing = {
        queues: [
            run["wall_seconds"]
            for run in runs
            if run["queues"] == queues
            and (run["repeat"] > 0 or args.timing_only)
        ]
        for queues in queue_modes
    }
    # The traced correctness run is not a performance sample.
    speedup = (
        statistics.median(timing[1])
        / statistics.median(timing[queue_modes[1]])
        if timing[1]
        else None
    )
    evidence = {
        "measurement_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source_sha256": source_sha,
        "accelerator_cpu": args.accelerator_cpu,
        "accelerator_cores": args.accelerator_cores,
        "memory_controllers": args.memory_controllers,
        "timing_only": args.timing_only,
        "protocol": protocol,
        "runs": runs,
        "speedup": speedup,
        "timing_summary": {
            queues: {
                "samples": len(timing[queues]),
                "median_wall_seconds": statistics.median(timing[queues]),
                "simulated_clock_hz": runs[0]["simulation_clock_periods"]
                / statistics.median(timing[queues]),
            }
            for queues in queue_modes
            if timing[queues]
        },
        "performance_verified_idle": bool(timing[1])
        and all(
            run.get("idle_machine_checked", False)
            for run in runs
            if (run["repeat"] > 0 or args.timing_only)
        ),
        "quantum": args.transport_delay,
        "transport_delay": args.transport_delay,
        "machine": {
            "hostname": platform.node(),
            "platform": platform.platform(),
            "lscpu": subprocess.check_output(["lscpu"], text=True),
            "cpu_affinity": sorted(os.sched_getaffinity(0)),
            "pid_namespace": os.readlink("/proc/self/ns/pid"),
        },
        "comparison": compare(
            args.output / "q1-0",
            args.output / f"q{queue_modes[1]}-0",
            traces=not args.timing_only,
        ),
        "gem5_sha256": gem5_sha,
        "firmware_sha256": firmware_sha,
    }
    (args.output / "validation.json").write_text(
        json.dumps(evidence, indent=2) + "\n"
    )
    print(
        f"PASS: {args.accelerator_cores}-core smoke, golden, clocks, "
        f"timing; speedup={speedup}"
    )


if __name__ == "__main__":
    main()
