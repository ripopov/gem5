# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
"""Measure host rendering with eight cached O3 accelerators waiting in WFI."""

import argparse
import json
import re
import struct
import subprocess
import time
from pathlib import Path

from experiment import (
    ROOT,
    machine_info,
    measured_run,
    stats_blocks,
    symbols,
)
from PIL import Image
from validate import (
    analytic_smoke,
    scene_data,
)
from validate_backend import (
    GOLDEN,
    check_accelerators,
    check_memory,
    check_transport,
)
from validate_reduction import digest


def inspect(directory):
    log = (directory / "run.log").read_text()
    assert re.search(r"Stopped at \d+: m5_exit instruction encountered", log)
    parameters = json.loads((directory / "parameters.json").read_text())
    expected = {
        "host_render": True,
        "backend": "gem5",
        "accelerator_cores": 8,
        "accelerator_cpu": "o3",
        "accelerator_caches": "l1",
        "memory_controllers": 4,
        "queues": 1,
        "render_mode": 1,
        "samples_per_pixel": 1,
        "max_secondary_depth": 0,
        "seed": 1,
    }
    assert all(parameters[name] == value for name, value in expected.items())
    config = json.loads((directory / "config.json").read_text())
    quantum = check_transport(config, parameters["transport_delay"])
    system = config["system"]
    memories = check_memory(system, 4)
    check_accelerators(system, 1, "o3", 8)
    assert system["cpu"]["type"] == "BaseO3CPU"
    assert system["cpu"]["eventq_index"] == 0
    assert system["clk_domain"]["clock"] == [1000]
    assert system["host_cache"]["size"] == 65536
    assert system["host_cache"]["assoc"] == 8
    host = symbols(ROOT / "util/rt_reference/out/host-render.elf")
    assert struct.unpack("<QQ8IQ", (directory / "job.bin").read_bytes()) == (
        host["scene_bytes"],
        host["pixels"],
        384,
        0,
        0,
        96,
        96,
        1,
        1,
        0,
        1,
    )
    rgba = (directory / "image.rgba").read_bytes()
    assert (
        len(rgba) == 96 * 96 * 4 and digest(directory / "image.rgba") == GOLDEN
    )
    bmp = Image.open(directory / "image.bmp").convert("RGB")
    assert bmp.size == (96, 96)
    assert bmp.tobytes() == bytes(v for i, v in enumerate(rgba) if i % 4 != 3)
    error = analytic_smoke(
        scene_data((directory / "scene.bin").read_bytes()), rgba
    )
    blocks = stats_blocks(directory)
    assert len(blocks) == 2
    batch = blocks[0]
    start = int(batch["finalTick"] - batch["simTicks"])
    assert 0 < start < batch["finalTick"] < blocks[-1]["finalTick"]
    quiesce = (directory / "wfi.log").read_text()
    idle = []
    for i in range(8):
        prefix = f"system.accelerators{i}"
        events = re.findall(
            rf"^\s*(\d+): {re.escape(prefix)}\.cpu: (.+)$",
            quiesce,
            re.MULTILINE,
        )
        assert len(events) == 2, (prefix, events)
        quiesce_tick, call = events[0]
        tick, event = events[1]
        assert call == "quiesce()" and event == "Suspending Context"
        assert quiesce_tick == tick and int(tick) < start
        for block in blocks:
            assert block[f"{prefix}.cpu.numCycles"] == 0
            assert block.get(f"{prefix}.cpu.commitStats0.numInsts", 0) == 0
            for counter in (
                "readRequests",
                "writeRequests",
                "cleanRequests",
                "bytesCopied",
                "busyTicks",
                "errors",
            ):
                assert block.get(f"{prefix}.control.{counter}", 0) == 0
            for memory in memories:
                adapter = system["adapters"][i]["name"]
                for counter in ("bytesRead", "bytesWritten"):
                    assert (
                        block.get(f"{memory['path']}.{counter}::{adapter}", 0)
                        == 0
                    )
            for tcm in ("code_tcm", "data_tcm"):
                for counter in ("bytesRead", "bytesInstRead", "bytesWritten"):
                    assert (
                        block.get(f"{prefix}.{tcm}.{counter}::total", 0) == 0
                    )
        idle.append(
            {
                "core": i,
                "wfi_suspend_tick": int(tick),
                "render_retired_instructions": 0,
                "render_cpu_cycles": 0,
                "render_dma_requests": 0,
                "wakeup_events": 0,
            }
        )
    cache = {
        counter: int(batch.get(f"system.host_cache.{counter}::total", 0))
        for counter in (
            "overallHits",
            "overallMisses",
            "overallMshrHits",
            "overallMshrMisses",
        )
    }
    accesses = cache["overallHits"] + cache["overallMisses"]
    assert accesses > 0
    retired = int(batch["system.cpu.commitStats0.numInsts"])
    cycles = int(batch["system.cpu.numCycles"])
    elapsed = int(batch["simTicks"] / 1000)
    assert retired > 0 and cycles > 0
    controllers = [
        {
            "path": memory["path"],
            "read_bytes": int(batch[f"{memory['path']}.bytesRead::total"]),
            "write_bytes": int(batch[f"{memory['path']}.bytesWritten::total"]),
        }
        for memory in memories
    ]
    return {
        "renderer": "Host",
        "quantum_ticks": quantum,
        "transport_delay": parameters["transport_delay"],
        "accelerators": 8,
        "active_render_cpus": 1,
        "idle_accelerators": idle,
        "queues": 1,
        "memory_controllers": 4,
        "full_simulated_cycles": int(blocks[-1]["finalTick"] / 1000),
        "batch_simulated_cycles": elapsed,
        "batch_retired_instructions": retired,
        "batch_counted_cpu_cycles": cycles,
        "weighted_core_ipc": retired / cycles,
        "cluster_ipc": retired / elapsed,
        "batch_megapixels_per_simulated_second": 9216 * 1000 / elapsed,
        "counted_cpu_occupancy_percent": 100 * cycles / elapsed,
        "cache": {
            "icache": None,
            "dcache": {
                "kind": "unified host cache",
                "hits": cache["overallHits"],
                "misses": cache["overallMisses"],
                "hit_percent": 100 * cache["overallHits"] / accesses,
                "mshr_hits": cache["overallMshrHits"],
                "mshr_misses": cache["overallMshrMisses"],
                "mshr_miss_percent": 100
                * cache["overallMshrMisses"]
                / accesses,
            },
        },
        "dma_batch_duty_percent": 0,
        "dma_copied_bytes": 0,
        "ram_read_bytes": sum(m["read_bytes"] for m in controllers),
        "ram_write_bytes": sum(m["write_bytes"] for m in controllers),
        "controller_statistics": controllers,
        "stats_region_start_tick": start,
        "stats_region_end_tick": int(batch["finalTick"]),
        "rgba_sha256": GOLDEN,
        "analytic_max_byte_error": error,
        "output_sha256": {
            name: digest(directory / name)
            for name in (
                "image.rgba",
                "image.bmp",
                "scene.bin",
                "job.bin",
                "parameters.bin",
            )
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--inspect-only", action="store_true")
    parser.add_argument(
        "--gem5", type=Path, default=ROOT / "build/RISCV/gem5.opt"
    )
    parser.add_argument(
        "--reference",
        type=Path,
        default=ROOT / "util/rt_reference/RAYTRACE_RESULTS.json",
        help="Accelerator validation or report JSON whose executable and "
        "accelerator firmware must match the host measurement",
    )
    parser.add_argument("--transport-delay", default="5ns")
    args = parser.parse_args()
    directory = args.output.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    previous = json.loads(args.reference.read_text())
    assert previous["quantum"] == args.transport_delay
    assert previous["transport_delay"] == args.transport_delay
    assert digest(args.gem5) == previous["gem5_sha256"]
    accelerator = ROOT / "util/rt_reference/out/accelerator.elf"
    assert (
        digest(accelerator) == previous["firmware_sha256"]["accelerator.elf"]
    )
    files = [ROOT / name for name in previous["source_sha256"]]
    files.append(Path(__file__).resolve())
    provenance = {str(p.relative_to(ROOT)): digest(p) for p in files}
    firmware_sha = {
        name: digest(ROOT / "util/rt_reference/out" / name)
        for name in ("host-render.elf", "accelerator.elf")
    }
    manifest_path = directory / "measurement-artifacts.json"
    if args.inspect_only:
        manifest = json.loads(manifest_path.read_text())
        saved_path = directory / "validation.json"
        if saved_path.exists():
            saved = json.loads(saved_path.read_text())
            assert saved["inspection_source_sha256"] == provenance
    else:
        assert not (directory / "wall.json").exists()
        manifest = {
            "source_sha256": provenance,
            "gem5_sha256": digest(args.gem5),
            "firmware_sha256": firmware_sha,
            "captured_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "capture_stage": "before_simulation",
        }
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
        command = [
            str(args.gem5.resolve()),
            f"--outdir={directory}",
            "--debug-flags=Quiesce",
            "--debug-ignore=system.cpu*",
            "--debug-file=wfi.log",
            str(ROOT / "configs/example/rt_accel.py"),
            "--host-render",
            "--accelerator-cores=8",
            "--accelerator-cpu=o3",
            "--queues=1",
            "--render-mode=1",
            "--samples-per-pixel=1",
            "--max-secondary-depth=0",
            "--seed=1",
            f"--transport-delay={args.transport_delay}",
        ]
        print("Running:", " ".join(command), flush=True)
        measured_run(command, directory, require_idle_machine=True)
    assert manifest["gem5_sha256"] == digest(args.gem5)
    assert manifest["firmware_sha256"] == firmware_sha
    # An inspector-only fix can recheck a completed sample. Require every
    # simulator/firmware/configuration source to retain its measured hash.
    inspector_name = str(Path(__file__).resolve().relative_to(ROOT))
    for name, value in manifest["source_sha256"].items():
        if name != inspector_name:
            assert provenance[name] == value, name
    result = inspect(directory)
    assert result["transport_delay"] == args.transport_delay
    result.update(json.loads((directory / "wall.json").read_text()))
    result["full_simulated_cycles_per_wall_second"] = (
        result["full_simulated_cycles"] / result["wall_seconds"]
    )
    assert provenance == {str(p.relative_to(ROOT)): digest(p) for p in files}
    result.update(
        source_sha256=manifest["source_sha256"],
        inspection_source_sha256=provenance,
        gem5_sha256=digest(args.gem5),
        firmware_sha256={
            "host-render.elf": digest(
                ROOT / "util/rt_reference/out/host-render.elf"
            ),
            "accelerator.elf": digest(accelerator),
        },
        machine=machine_info(),
        base_commit=subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        measurement_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        artifact_directory=str(directory.relative_to(ROOT)),
        raw_artifact_sha256={
            name: digest(directory / name)
            for name in (
                "stats.txt",
                "config.json",
                "wall.json",
                "run.log",
                "wfi.log",
            )
        },
    )
    (directory / "validation.json").write_text(
        json.dumps(result, indent=2) + "\n"
    )
    print(
        f"PASS: host smoke; all 8 accelerator CPUs stayed in WFI; "
        f"wall={result['wall_seconds']:.3f}s",
        flush=True,
    )


if __name__ == "__main__":
    main()
