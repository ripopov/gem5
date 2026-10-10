# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
"""Validate reduction/protocol/throughput and measure serial versus PDES."""

import argparse
import hashlib
import json
import statistics
import struct
import subprocess
from datetime import (
    datetime,
    timezone,
)
from pathlib import Path

from experiment import (
    ROOT,
    compare_statistics,
    machine_info,
    measured_run,
    stats_blocks,
    symbols,
)

INPUT_BASE = 0x82000000
REGION_BYTES = 1 << 20
COPY_BASE = 0x83100000
PATTERN_BASE = 0x83000000
CONFIG = ROOT / "configs/example/rt_accel_reduction.py"


def digest(path):
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def artifact_files(gem5):
    return [
        gem5,
        *(
            ROOT / "util/rt_reference/out" / name
            for name in ("reduction-host.elf", "reduction-accelerator.elf")
        ),
        *(
            ROOT / path
            for path in (
                "configs/example/rt_accel_system.py",
                "configs/example/rt_accel_reduction.py",
                "src/dev/rt_accel/control.cc",
                "src/dev/rt_accel/control.hh",
                "src/dev/rt_accel/code_tcm.cc",
                "src/dev/rt_accel/code_tcm.hh",
                "src/dev/rt_accel/RTAccelerator.py",
                "util/rt_reference/reduction.h",
                "util/rt_reference/reduction_host.c",
                "util/rt_reference/reduction_accelerator.c",
                "util/rt_reference/host_runtime.c",
                "util/rt_reference/host_runtime.h",
                "util/rt_reference/accelerator_runtime.c",
                "util/rt_reference/accelerator_runtime.h",
                "util/rt_reference/Makefile",
                "util/rt_reference/start.S",
                "util/rt_reference/link.ld",
                "util/rt_reference/accelerator.ld",
                "util/rt_reference/experiment.py",
                "util/rt_reference/validate_reduction.py",
            )
        ),
    ]


def prefix(index, cores):
    return "system.accelerators" + (str(index) if cores > 1 else "")


def wire_evidence(directory, jobs, workload):
    evidence = []
    for index, (source, destination, length, mode) in enumerate(jobs):
        read, written, read_ticks, write_ticks = set(), set(), [], []
        outstanding = set()
        peak_reads = peak_writes = peak = 0
        result_responses, msi = [], []
        descriptor = None
        expected_output = (
            struct.pack("<f", 262144.0)
            if workload == "reduction"
            else (directory / "copy.bin").read_bytes()
        )
        for line in (
            (directory / f"accel{index}.trace").read_text().splitlines()
        ):
            tick, stage, command, address, size, data = line.split()
            tick, address, size = int(tick), int(address, 16), int(size)
            key = (command.replace("Resp", "Req"), address, size)
            if stage == "master_accept":
                assert key not in outstanding
                outstanding.add(key)
                reads = sum(item[0] == "ReadReq" for item in outstanding)
                writes = sum(item[0] == "WriteReq" for item in outstanding)
                peak_reads, peak_writes = max(peak_reads, reads), max(
                    peak_writes, writes
                )
                peak = max(peak, len(outstanding))
                extent = set(range(address, address + size))
                if command == "ReadReq":
                    if source <= address < source + length:
                        assert (
                            source <= address
                            and address + size <= source + length
                        )
                        assert not extent & read or (
                            workload == "dma-test"
                            and address == source
                            and size == 1
                        )
                        read.update(extent)
                        if size == 64:
                            read_ticks.append((address, tick))
                    else:
                        assert descriptor is None and size == 24
                        descriptor = address
                elif address == 0x20000000 + 4 * index:
                    assert (
                        size == 4
                        and data == struct.pack("<I", index + 1).hex()
                    )
                    msi.append(tick)
                else:
                    output_size = 4 if workload == "reduction" else length
                    assert (
                        destination <= address
                        and address + size <= destination + output_size
                    )
                    assert not extent & written
                    written.update(extent)
                    assert (
                        bytes.fromhex(data)
                        == expected_output[
                            address
                            - destination : address
                            - destination
                            + size
                        ]
                    )
                    if size == 64:
                        write_ticks.append((address, tick))
            elif stage == "master_arrival":
                assert key in outstanding
                outstanding.remove(key)
                if (
                    command == "ReadResp"
                    and source <= address < source + length
                ):
                    if workload == "dma-test":
                        expected = bytes(
                            (37 * i + i // 64) & 255
                            for i in range(
                                address - PATTERN_BASE,
                                address - PATTERN_BASE + size,
                            )
                        )
                    else:
                        expected = struct.pack("<f", 1.0) * (size // 4)
                    assert bytes.fromhex(data) == expected
                if (
                    command == "WriteResp"
                    and destination <= address < destination + length
                ):
                    result_responses.append(tick)
        assert not outstanding
        assert read == set(range(source, source + length))
        assert written == set(
            range(
                destination,
                destination + (4 if workload == "reduction" else length),
            )
        )
        assert descriptor is not None and len(msi) == 1 and result_responses
        assert msi[0] > max(
            result_responses
        ), "MSI preceded destination visibility"
        visible = [
            int(line.split()[0])
            for line in (directory / "msi.trace").read_text().splitlines()
            if f"msi_access WriteReq {0x20000000 + 4 * index:x} " in line
        ]
        assert len(visible) == 1 and visible[0] > msi[0]
        status_reads = [
            int(line.split()[0])
            for line in (directory / f"host{index}.trace")
            .read_text()
            .splitlines()
            if f"mmio_accept ReadReq {0x10000014 + 0x1000 * index:x} " in line
        ]
        assert len(status_reads) == 1 and status_reads[0] > visible[0]
        evidence.append(
            {
                "peak_outstanding_reads": peak_reads,
                "peak_outstanding_writes": peak_writes,
                "peak_transport_requests": peak,
                "msi_visible_tick": visible[0],
                "last_result_response_tick": max(result_responses),
                "read_sustained_bytes_per_cycle": sustained_rate(read_ticks),
                "write_sustained_bytes_per_cycle": sustained_rate(write_ticks),
            }
        )
    return evidence


def fault_wire_evidence(directory, queue_depth):
    """Check read/write failures, draining, recovery and MSI visibility."""
    live = {}
    peaks = {"ReadReq": 0, "WriteReq": 0}
    failures = []
    recovery_reads = []
    recovery_writes = []
    msi = []
    for line in (directory / "accel0.trace").read_text().splitlines():
        tick, stage, cmd, address, size, _ = line.split()
        tick, address, size = int(tick), int(address, 16), int(size)
        key = (address, size)
        if stage == "master_accept":
            assert key not in live
            live[key] = cmd
            for kind in peaks:
                peaks[kind] = max(peaks[kind], list(live.values()).count(kind))
            if address == INPUT_BASE:
                assert not live.keys() & {
                    (0x83F00000 + i * 64, 64) for i in range(64)
                }
                recovery_reads.append(tick)
            elif address == COPY_BASE:
                assert not live.keys() & {
                    (0x83F00000 + i * 64, 64) for i in range(64)
                }
                recovery_writes.append(tick)
            elif address == 0x20000000:
                assert len(live) == 1
                msi.append(tick)
        elif stage == "master_arrival":
            kind = live.pop(key)
            if cmd == "BadAddressError":
                assert 0x83F00000 <= address < 0x83F01000
                failures.append((tick, kind))
            else:
                assert cmd == kind.replace("Req", "Resp")
    assert not live
    expected = min(queue_depth, 64)
    assert len(failures) == 2 * expected
    assert peaks["ReadReq"] == peaks["WriteReq"] == expected
    assert len(recovery_reads) == len(recovery_writes) == len(msi) == 1
    assert recovery_reads[0] > max(
        tick for tick, kind in failures if kind == "ReadReq"
    )
    assert recovery_writes[0] > max(
        tick for tick, kind in failures if kind == "WriteReq"
    )
    assert msi[0] > recovery_writes[0]
    return {
        "error_responses": len(failures),
        "peak_outstanding": peaks,
        "last_error_tick": max(tick for tick, _ in failures),
        "recovery_write_tick": recovery_writes[0],
        "msi_issue_tick": msi[0],
    }


def sustained_rate(transactions):
    """Central 512 packets of the first 64 KiB tile; 1 cycle = 1000 ticks.

    Fixed address window excludes pipeline fill/drain and tile boundaries.
    It is selected before observing timings, not a fastest-window search.
    """
    if len(transactions) < 768:
        return None
    first = transactions[0][0]
    window = [
        tick
        for address, tick in transactions
        if first + 256 * 64 <= address < first + 768 * 64
    ]
    assert len(window) == 512
    return 64 * (len(window) - 1) * 1000 / (window[-1] - window[0])


def inspect(directory, workload, queues):
    assert (
        "m5_exit instruction encountered"
        in (directory / "run.log").read_text()
    )
    parameters = json.loads((directory / "parameters.json").read_text())
    system = json.loads((directory / "config.json").read_text())["system"]
    cores = parameters.get(
        "accelerator_cores", 16 if workload == "reduction" else 1
    )
    assert (
        parameters["workload"] == workload and parameters["queues"] == queues
    )
    assert (
        len(system["accelerators"]) == cores
        and len(system["adapters"]) == cores
    )
    assert system["eventq_index"] == system["cpu"]["eventq_index"] == 0
    assert system["clk_domain"]["clock"] == [1000]
    controller_count = parameters.get("memory_controllers", 1)
    memories = [
        system["memory" if i == 0 else f"memory{i}"]
        for i in range(controller_count)
    ]
    assert system["memories"] == [memory["path"] for memory in memories]
    storage_end = 0x83F00000 if workload == "dma-fault" else 0x84000000
    for i, memory in enumerate(memories):
        assert memory["eventq_index"] == 0
        assert memory["clk_domain"] == "system.clk_domain"
        assert memory["latency"] == parameters["memory_cycles"] * 1000
        assert memory["bandwidth"] == memories[0]["bandwidth"]
        expected_range = f"{0x80000000}:{storage_end}"
        if controller_count > 1:
            interleave_bits = controller_count.bit_length() - 1
            masks = ":".join(
                str(1 << (6 + bit)) for bit in range(interleave_bits)
            )
            expected_range += f":{i}:{masks}"
        assert memory["range"] == expected_range
    assert system["membus"]["width"] == 64
    assert system["memory_router_clock"]["clock"] == [250]
    for i, accel in enumerate(system["accelerators"]):
        queue = i + 1 if queues != 1 else 0
        assert accel["eventq_index"] == queue
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
        assert accel["clk_domain"]["clock"] == [1000]
        cached = parameters.get("accelerator_caches", "none") == "l1"
        assert accel["control"]["coherent_dma"] == cached
        assert accel["bus"]["type"] == (
            "CoherentXBar" if cached else "NoncoherentXBar"
        )
        if cached:
            for name, size, ways, mshrs, targets in (
                ("icache", 8192, 2, 4, 16),
                ("dcache", 16384, 4, 16, 32),
            ):
                cache = accel[name]
                assert cache["eventq_index"] == queue
                assert cache["clk_domain"] == accel["clk_domain"]["path"]
                assert cache["size"] == size and cache["assoc"] == ways
                assert cache["mshrs"] == mshrs
                assert cache["tgts_per_mshr"] == targets
                assert cache["tag_latency"] == cache["data_latency"] == 1
        assert accel["data_tcm"]["range"] == "1073807360:1074003968"
        assert (
            accel["control"]["max_outstanding"]
            == parameters["dma_outstanding"]
        )
        assert accel["wrapper"]["queue_depth"] == parameters["queue_depth"]
        assert accel["cpu"]["numThreads"] == 1
        assert accel["cpu"]["type"] == (
            "BaseO3CPU"
            if parameters["accelerator_cpu"] == "o3"
            else "BaseTimingSimpleCPU"
        )
        assert system["adapters"][i]["eventq_index"] == 0
    image = (directory / "inputs.elf").read_bytes()
    assert image[4096 : 4096 + (16 << 20)] == struct.pack("<f", 1.0) * (
        4 << 20
    )
    raw = (directory / "jobs.bin").read_bytes()
    assert len(raw) == cores * 64
    jobs = [
        struct.unpack("<QQII", raw[64 * i : 64 * i + 24]) for i in range(cores)
    ]
    host = symbols(ROOT / "util/rt_reference/out/reduction-host.elf")
    for i, job in enumerate(jobs):
        if workload == "reduction":
            assert job == (
                INPUT_BASE + i * REGION_BYTES,
                host["partials"] + 4 * i,
                REGION_BYTES,
                0,
            )
        elif workload == "copy":
            assert job == (INPUT_BASE, COPY_BASE, REGION_BYTES, 1)
        elif workload == "dma-fault":
            assert job == (INPUT_BASE, COPY_BASE, 4096, 3)
        else:
            assert job == (PATTERN_BASE + 17, COPY_BASE + 5, 8193, 2)
    if workload == "reduction":
        assert (
            struct.unpack(
                f"<{cores}f", (directory / "partials.bin").read_bytes()
            )
            == (262144.0,) * cores
        )
        assert struct.unpack("<f", (directory / "sum.bin").read_bytes()) == (
            float(cores * REGION_BYTES // 4),
        )
    elif workload == "copy":
        assert (directory / "copy.bin").read_bytes() == struct.pack(
            "<f", 1.0
        ) * (REGION_BYTES // 4)
    elif workload == "dma-fault":
        assert (directory / "copy.bin").read_bytes() == struct.pack("<f", 1.0)
    else:
        assert (directory / "copy.bin").read_bytes() == bytes(
            (37 * i + i // 64) & 255 for i in range(17, 17 + 8193)
        )
    blocks = stats_blocks(directory)
    assert len(blocks) == 2
    batch = blocks[0]
    layout = symbols(ROOT / "util/rt_reference/out/reduction-accelerator.elf")
    assert (
        layout["__code_end"] <= 0x40010000
        and layout["__stack_top"] <= 0x40040000
    )
    dma = []
    for i in range(cores):
        name = system["adapters"][i]["name"]
        assert sum(
            batch.get(f"{m['path']}.bytesRead::{name}", 0) for m in memories
        ) == (
            28
            if workload == "dma-fault"
            else jobs[i][2] + 24 + (1 if workload == "dma-test" else 0)
        )
        assert sum(
            batch.get(f"{m['path']}.bytesWritten::{name}", 0) for m in memories
        ) == (4 if workload in ("reduction", "dma-fault") else jobs[i][2])
        if workload == "reduction" and controller_count > 1:
            for channel, memory in enumerate(memories):
                channel_mask = controller_count - 1
                descriptor_channel = (
                    (host["jobs"] + i * 64) >> 6
                ) & channel_mask
                output_channel = (jobs[i][1] >> 6) & channel_mask
                assert batch.get(
                    f"{memory['path']}.bytesRead::{name}", 0
                ) == REGION_BYTES // controller_count + 24 * (
                    descriptor_channel == channel
                )
                assert batch.get(
                    f"{memory['path']}.bytesWritten::{name}", 0
                ) == 4 * (output_channel == channel)
        p = prefix(i, cores) + ".control."
        copied = batch[p + "bytesCopied"]
        assert copied == (
            32
            if workload == "dma-fault"
            else (
                REGION_BYTES + 28
                if workload == "reduction"
                else (
                    2 * REGION_BYTES + 24
                    if workload == "copy"
                    else 2 * 8193 + 33
                )
            )
        )
        assert batch[p + "errors"] == (
            4
            if workload == "dma-test"
            else 2 if workload == "dma-fault" else 0
        )
        assert 1 < batch[p + "peakChunks"] <= parameters["dma_outstanding"]
        dma.append(
            {
                "bytes_copied": copied,
                "busy_ticks": batch[p + "busyTicks"],
                "average_bytes_per_busy_cycle": copied
                * 1000
                / batch[p + "busyTicks"],
                "peak_chunks": batch[p + "peakChunks"],
                "read_retries": batch[p + "readRetries"],
                "write_retries": batch[p + "writeRetries"],
            }
        )
    wire = (
        (
            fault_wire_evidence(directory, parameters["queue_depth"])
            if workload == "dma-fault"
            else wire_evidence(directory, jobs, workload)
        )
        if parameters["trace"]
        else None
    )
    result = {
        "workload": workload,
        "accelerator_cores": cores,
        "queues": queues,
        "memory_controllers": controller_count,
        "controller_statistics": [
            {
                "path": memory["path"],
                "range": memory["range"],
                "latency_ticks": memory["latency"],
                "ticks_per_byte": float(memory["bandwidth"]),
                "dma_read_bytes": sum(
                    batch.get(
                        f"{memory['path']}.bytesRead::{adapter['name']}", 0
                    )
                    for adapter in system["adapters"]
                ),
                "dma_write_bytes": sum(
                    batch.get(
                        f"{memory['path']}.bytesWritten::{adapter['name']}", 0
                    )
                    for adapter in system["adapters"]
                ),
            }
            for memory in memories
        ],
        "batch_start_tick": batch["finalTick"] - batch["simTicks"],
        "batch_end_tick": batch["finalTick"],
        "batch_ticks": batch["simTicks"],
        "final_tick": blocks[-1]["finalTick"],
        "dma": dma,
        "wire": wire,
        "aggregate_input_bytes_per_batch_cycle": cores
        * jobs[0][2]
        * 1000
        / batch["simTicks"],
        "code_tcm_bytes": layout["__code_end"] - 0x40000000,
        "data_tcm_bytes_including_stack": layout["__stack_top"] - 0x40010000,
        "simulation_wall": json.loads(
            (directory / "simulation-wall.json").read_text()
        ),
        "process_wall": json.loads((directory / "wall.json").read_text()),
    }
    (directory / "validation.json").write_text(
        json.dumps(result, indent=2) + "\n"
    )
    return result


def compare_outputs(left, right, workload):
    files = ["jobs.bin", "parameters.bin", "inputs.elf"]
    if (left / "boot.elf").is_file():
        files.append("boot.elf")
    files += (
        ["partials.bin", "sum.bin"]
        if workload == "reduction"
        else ["copy.bin"]
    )
    for name in files:
        assert digest(left / name) == digest(right / name), name
    return {name: digest(left / name) for name in files}


def compare(left, right, workload, traces):
    compare_outputs(left, right, workload)
    result = compare_statistics(left, right)
    result.update(workload=workload, left=left.name, right=right.name)
    if traces:
        parameters = json.loads((left / "parameters.json").read_text())
        cores = parameters.get(
            "accelerator_cores", 16 if workload == "reduction" else 1
        )
        names = ["msi.trace"] + [
            f"{kind}{i}.trace"
            for kind in ("host", "accel")
            for i in range(cores)
        ]
        result["trace_sha256"] = {}
        for name in names:
            assert digest(left / name) == digest(right / name), name
            result["trace_sha256"][name] = digest(left / name)
    return result


def run_quick(args, provenance):
    """Time untraced DES/PDES pairs at the current reduction defaults."""
    groups = []
    for cores in (8, 16):
        runs, comparisons = [], []
        parallel_queues = cores + 1
        for repeat in range(args.runs):
            queues_order = (
                (1, parallel_queues)
                if repeat % 2 == 0
                else (parallel_queues, 1)
            )
            for queues in queues_order:
                name = f"n{cores}-timing-{repeat}-q{queues}"
                directory = args.output / name
                directory.mkdir(exist_ok=True)
                command = [
                    str(args.gem5.resolve()),
                    f"--outdir={directory}",
                    str(CONFIG),
                    "--workload=reduction",
                    f"--accelerator-cores={cores}",
                    f"--queues={queues}",
                ]
                if not args.inspect_only:
                    assert not (
                        directory / "wall.json"
                    ).exists(), "Choose a fresh output directory"
                    print("Running:", " ".join(command), flush=True)
                    measured_run(command, directory, require_idle_machine=True)
                config = json.loads((directory / "config.json").read_text())
                assert config["sim_quantum"] == 5000
                for accel in config["system"]["accelerators"]:
                    wrapper = accel["wrapper"]
                    assert (
                        wrapper["request_delay"]
                        == wrapper["response_delay"]
                        == 5000
                    )
                result = inspect(directory, "reduction", queues)
                result["run_name"] = name
                runs.append(result)
                print(
                    f"Validated {name}: "
                    f"{result['simulation_wall']['wall_seconds']:.3f} s",
                    flush=True,
                )
            comparisons.append(
                compare(
                    args.output / f"n{cores}-timing-{repeat}-q1",
                    args.output
                    / f"n{cores}-timing-{repeat}-q{parallel_queues}",
                    "reduction",
                    False,
                )
            )
        simulation, process = {}, {}
        for queues in (1, parallel_queues):
            selected = [run for run in runs if run["queues"] == queues]
            simulation[queues] = statistics.median(
                run["simulation_wall"]["wall_seconds"] for run in selected
            )
            process[queues] = statistics.median(
                run["process_wall"]["wall_seconds"] for run in selected
            )
        groups.append(
            {
                "accelerator_cores": cores,
                "input_bytes": cores * REGION_BYTES,
                "runs": runs,
                "comparisons": comparisons,
                "median_simulation_wall_seconds": simulation,
                "median_process_wall_seconds": process,
                "pdes_speedup": simulation[1] / simulation[parallel_queues],
                "process_speedup": process[1] / process[parallel_queues],
            }
        )
    assert provenance == {
        str(path.resolve()): digest(path) for path in artifact_files(args.gem5)
    }, "Inputs/executable/source changed during measurement"
    evidence = {
        "evaluation": "quick-reduction",
        "measured_at_utc": datetime.now(timezone.utc).isoformat(),
        "git_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "artifact_sha256": provenance,
        "machine": machine_info(),
        "transport_delay_ns": 5,
        "quantum_ticks": 5000,
        "tick_frequency_hz": 10**12,
        "memory_controllers": 4,
        "memory_bandwidth_per_controller": "32GiB/s",
        "memory_latency_ns": 100,
        "groups": groups,
    }
    (args.output / "results.json").write_text(
        json.dumps(evidence, indent=2) + "\n"
    )
    for group in groups:
        print(
            f"PASS: {group['accelerator_cores']} accelerators; "
            f"PDES speedup={group['pdes_speedup']:.3f}x",
            flush=True,
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--gem5", type=Path, default=ROOT / "build/RISCV/gem5.opt"
    )
    parser.add_argument(
        "--output", type=Path, default=ROOT / "m5out/reduction"
    )
    parser.add_argument(
        "--runs", type=int, default=3, help="Untraced timing pairs"
    )
    parser.add_argument("--inspect-only", action="store_true")
    parser.add_argument(
        "--quick",
        action="store_true",
        help="Only untraced 8/16-core DES/PDES pairs at current defaults",
    )
    args = parser.parse_args()
    assert args.runs >= 1
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    provenance_files = artifact_files(args.gem5)
    provenance = {
        str(path.resolve()): digest(path) for path in provenance_files
    }
    if args.inspect_only:
        saved = json.loads((args.output / "results.json").read_text())
        assert (
            saved["artifact_sha256"] == provenance
        ), "Current artifacts do not match the recorded experiment"
    if args.quick:
        run_quick(args, provenance)
        return
    runs, comparisons = [], []

    def run(workload, queues, name, trace=False, extra=()):
        directory = args.output / name
        directory.mkdir(exist_ok=True)
        command = [
            str(args.gem5.resolve()),
            f"--outdir={directory}",
            str(CONFIG),
            f"--workload={workload}",
            f"--queues={queues}",
            # Retain the memory-bound baseline and bandwidth sensitivity
            # check independently of the direct-run default.
            "--memory-controllers=1",
            *extra,
        ]
        if trace:
            command.append("--trace")
        if not args.inspect_only:
            assert not (
                directory / "wall.json"
            ).exists(), "Choose a fresh output directory"
            print("Running:", " ".join(command), flush=True)
            measured_run(command, directory, require_idle_machine=True)
        result = inspect(directory, workload, queues)
        result["run_name"] = name
        runs.append(result)
        return result

    # Low transport credits force read and write retries; nonuniform input
    # exposes reordering/corruption that a buffer of identical 1.0f cannot.
    for queues in (1, 2):
        protocol = run(
            "dma-test",
            queues,
            f"protocol-q{queues}",
            True,
            ("--queue-depth=2", "--dma-outstanding=16"),
        )
        assert (
            sum(
                d["read_retries"] + d["write_retries"] for d in protocol["dma"]
            )
            > 0
        )
    comparisons.append(
        compare(
            args.output / "protocol-q1",
            args.output / "protocol-q2",
            "dma-test",
            True,
        )
    )
    for queues in (1, 2):
        run("dma-fault", queues, f"fault-q{queues}", True)
    comparisons.append(
        compare(
            args.output / "fault-q1",
            args.output / "fault-q2",
            "dma-fault",
            True,
        )
    )
    # Errors while both ports are rejected exercise cancellation of owned
    # packets as well as draining accepted transactions before recovery.
    for queues in (1, 2):
        fault = run(
            "dma-fault",
            queues,
            f"fault-backpressure-q{queues}",
            True,
            ("--queue-depth=2", "--dma-outstanding=16"),
        )
        assert all(
            d["read_retries"] > 0 and d["write_retries"] > 0
            for d in fault["dma"]
        )
    comparisons.append(
        compare(
            args.output / "fault-backpressure-q1",
            args.output / "fault-backpressure-q2",
            "dma-fault",
            True,
        )
    )
    probe = run("copy", 1, "throughput", True)
    for direction in ("read", "write"):
        rate = probe["wire"][0][f"{direction}_sustained_bytes_per_cycle"]
        assert (
            rate >= 63.0
        ), f"DMA {direction} misses 64 B/cycle target: {rate}"
    assert probe["wire"][0]["peak_outstanding_reads"] > 100
    assert probe["wire"][0]["peak_outstanding_writes"] > 100
    for queues in (1, 17):
        run("reduction", queues, f"correctness-q{queues}", True)
    comparisons.append(
        compare(
            args.output / "correctness-q1",
            args.output / "correctness-q17",
            "reduction",
            True,
        )
    )
    for repeat in range(args.runs):
        for queues in ((1, 17) if repeat % 2 == 0 else (17, 1)):
            run("reduction", queues, f"timing-{repeat}-q{queues}")
        comparisons.append(
            compare(
                args.output / f"timing-{repeat}-q1",
                args.output / f"timing-{repeat}-q17",
                "reduction",
                False,
            )
        )
        comparisons.append(
            compare(
                args.output / "correctness-q1",
                args.output / f"timing-{repeat}-q1",
                "reduction",
                False,
            )
        )
    wide = run(
        "reduction",
        1,
        "bandwidth-64GiB",
        False,
        ("--memory-bandwidth=64GiB/s",),
    )
    reference = next(run for run in runs if run["run_name"] == "timing-0-q1")
    sensitivity = reference["batch_ticks"] / wide["batch_ticks"]
    assert (
        sensitivity > 1.15
    ), "Bandwidth doubling did not improve completion time"
    timing = [run for run in runs if run["run_name"].startswith("timing-")]
    medians = {
        q: statistics.median(
            run["simulation_wall"]["wall_seconds"]
            for run in timing
            if run["queues"] == q
        )
        for q in (1, 17)
    }
    process_medians = {
        q: statistics.median(
            run["process_wall"]["wall_seconds"]
            for run in timing
            if run["queues"] == q
        )
        for q in (1, 17)
    }
    assert provenance == {
        str(path.resolve()): digest(path) for path in provenance_files
    }, "Inputs/executable/source changed during measurement"
    evidence = {
        "artifact_sha256": provenance,
        "git_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "bandwidth_doubling_completion_speedup": sensitivity,
        "runs": runs,
        "comparisons": comparisons,
        "median_simulation_wall_seconds": medians,
        "pdes_speedup": medians[1] / medians[17],
        "median_process_wall_seconds": process_medians,
        "process_speedup": process_medians[1] / process_medians[17],
        "gem5_sha256": digest(args.gem5),
        "firmware_sha256": {
            name: digest(ROOT / "util/rt_reference/out" / name)
            for name in ("reduction-host.elf", "reduction-accelerator.elf")
        },
        "machine": machine_info(),
    }
    (args.output / "results.json").write_text(
        json.dumps(evidence, indent=2) + "\n"
    )
    print(
        "PASS: reduction, wire timing, 64 B/cycle DMA; "
        f"PDES speedup={evidence['pdes_speedup']:.3f}"
    )


if __name__ == "__main__":
    main()
