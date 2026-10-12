# Copyright (c) 2026 Roman Popov
# SPDX-License-Identifier: BSD-3-Clause
"""Run the RISC-V O3 validation matrix and inspect guest-exported artifacts."""

import argparse
import concurrent.futures
import hashlib
import json
import math
import struct
import subprocess
import time
from pathlib import Path

from PIL import (
    Image,
    ImageDraw,
)

ROOT = Path(__file__).resolve().parents[2]
CASES = {
    "smoke": (1, 1, 0, 1),
    "primary": (0, 1, 0, 1),
    "depth1": (0, 1, 1, 1),
    "path4": (0, 4, 4, 1),
    "path1": (0, 1, 4, 1),
    "seed2": (0, 4, 4, 2),
    "repeat": (0, 4, 4, 1),
    "depth32": (0, 1, 32, 0xFFFFFFFFFFFFFFFF),
}


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def scale(a, s):
    return tuple(x * s for x in a)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    x, y, z = a
    u, v, w = b
    return y * w - z * v, z * u - x * w, x * v - y * u


def unit(a):
    return scale(a, 1 / math.sqrt(dot(a, a)))


def scene_data(data, width=96, height=96):
    m, n, scene_width, scene_height = struct.unpack_from("<4I", data)
    assert m == 8 and n == 278
    assert (scene_width, scene_height) == (width, height)
    assert len(data) == 56 + m * 24 + n * 40
    camera = struct.unpack_from("<10f", data, 16)
    assert all(math.isfinite(x) for x in camera)
    materials = [
        struct.unpack_from("<6f", data, 56 + i * 24) for i in range(m)
    ]
    for material in materials:
        assert all(math.isfinite(x) for x in material)
        assert all(0 <= x <= 1 for x in material[:3])
        assert all(x >= 0 for x in material[3:])
    objects = []
    for i in range(n):
        material, *vertices = struct.unpack_from(
            "<I9f", data, 56 + m * 24 + i * 40
        )
        assert material < m and all(math.isfinite(x) for x in vertices)
        a, b, c = vertices[:3], vertices[3:6], vertices[6:]
        e1, e2 = sub(b, a), sub(c, a)
        normal = cross(e1, e2)
        area = math.sqrt(dot(normal, normal)) / 2
        assert area > 0
        objects.append(
            (
                material,
                a,
                e1,
                e2,
                unit(normal),
                area,
                scale(add(a, add(b, c)), 1 / 3),
            )
        )
    assert sum(any(x > 0 for x in materials[o[0]][3:]) for o in objects) == 4
    return camera, materials, objects


def analytic_smoke(scene, image, width=96, height=96):
    """Independent double-precision linear triangle/shadow reference."""
    camera, materials, objects = scene
    position, target, up = camera[:3], camera[3:6], camera[6:9]
    forward = unit(sub(target, position))
    right = unit(cross(forward, up))
    up = cross(right, forward)
    half_height = math.tan(camera[9] * math.pi / 360)

    def trace(origin, direction, limit=1e30):
        best, selected = limit, -1
        for index, obj in enumerate(objects):
            _, vertex, e1, e2, *_ = obj
            p = cross(direction, e2)
            determinant = dot(e1, p)
            if abs(determinant) < 1e-8:
                continue
            s = sub(origin, vertex)
            u = dot(s, p) / determinant
            q = cross(s, e1)
            v = dot(direction, q) / determinant
            distance = dot(e2, q) / determinant
            if (
                0 <= u <= 1
                and v >= 0
                and u + v <= 1
                and 1e-4 < distance < best
            ):
                best, selected = distance, index
        return selected, best

    errors = []
    for y in range(5, height, 12):
        for x in range(5, width, 12):
            u = (2 * (x + 0.5) / width - 1) * half_height * width / height
            v = (1 - 2 * (y + 0.5) / height) * half_height
            direction = unit(add(forward, add(scale(right, u), scale(up, v))))
            selected, distance = trace(position, direction)
            result = [0.0, 0.0, 0.0]
            if selected >= 0:
                obj = objects[selected]
                material = materials[obj[0]]
                normal = obj[4]
                if dot(normal, direction) >= 0:
                    normal = scale(normal, -1)
                point = add(position, scale(direction, distance))
                origin = add(point, scale(normal, 1e-4))
                result = list(material[3:])
                for index, light in enumerate(objects):
                    emission = materials[light[0]][3:]
                    if index == selected or not any(emission):
                        continue
                    delta = sub(light[6], point)
                    squared = dot(delta, delta)
                    d = unit(delta)
                    cs, cl = max(0, dot(normal, d)), abs(dot(light[4], d))
                    shadow = sub(light[6], origin)
                    blocker, _ = trace(
                        origin,
                        unit(shadow),
                        math.sqrt(dot(shadow, shadow)) - 2e-4,
                    )
                    if blocker < 0:
                        factor = light[5] * cs * cl / (math.pi * squared)
                        result = [
                            a + b * e * factor
                            for a, b, e in zip(result, material[:3], emission)
                        ]
            expected = [int(255 * min(1, max(0, c)) + 0.5) for c in result]
            offset = 4 * (y * width + x)
            pixel = image[offset : offset + 3]
            errors.extend(abs(a - b) for a, b in zip(expected, pixel))
    assert max(errors) <= 1, f"analytic smoke differs: {max(errors)}"
    return max(errors)


def inspect(directory, case, symbols):
    log = (directory / "run.log").read_text()
    assert log.splitlines()[-1].endswith("m5_exit instruction encountered")
    parameters = json.loads((directory / "parameters.json").read_text())
    assert (
        tuple(
            parameters[name]
            for name in (
                "render_mode",
                "samples_per_pixel",
                "max_secondary_depth",
                "seed",
            )
        )
        == case
    )
    assert (directory / "parameters.bin").read_bytes() == struct.pack(
        "<IIIIQ", 0x52545031, *case
    )
    configuration = json.loads((directory / "config.json").read_text())
    system = configuration["system"]
    assert system["cpu"]["type"] == "BaseO3CPU"
    assert system["cpu"]["isa"][0]["type"] == "RiscvISA"
    assert len(system["accelerators"]) == len(system["adapters"]) == 8
    assert all(a["type"] == "RTAccelerator" for a in system["accelerators"])
    assert all(a["type"] == "RTAccelHostAdapter" for a in system["adapters"])
    assert system["host_cache"]["type"] == "Cache"
    assert system["membus"]["type"] == "NoncoherentXBar"
    stats = (directory / "stats.txt").read_text()
    host_seconds = [
        float(line.split()[1])
        for line in stats.splitlines()
        if line.startswith("hostSeconds ")
    ]
    assert len(host_seconds) == 1 and host_seconds[0] > 0
    assert "system.membus.pktCount::total" in stats
    # XBar counters suppress zeros. Any adapter traffic would appear here.
    assert not any(
        "system.membus.pktCount::" in line
        and ".adapters" in line
        and float(line.split()[1]) != 0
        for line in stats.splitlines()
    ), "Idle accelerators issued/received bus traffic"
    raw_scene = (directory / "scene.bin").read_bytes()
    scene = scene_data(raw_scene)
    raw_job = (directory / "job.bin").read_bytes()
    assert len(raw_job) == 56
    job = struct.unpack("<QQ8IQ", raw_job)
    assert job[:2] == (symbols["scene_bytes"], symbols["pixels"])
    assert job[2:7] == (384, 0, 0, 96, 96)
    assert job[7:] == case
    assert job[0] % 8 == 0 and job[1] % 4 == 0
    assert 0x80000000 <= job[0] < 0x84000000
    assert 0x80000000 <= job[1] < 0x84000000
    rgba = (directory / "image.rgba").read_bytes()
    assert len(rgba) == 96 * 96 * 4 and set(rgba[3::4]) == {255}
    bmp = Image.open(directory / "image.bmp").convert("RGB")
    assert bmp.size == (96, 96)
    assert bmp.tobytes() == bytes(c for i, c in enumerate(rgba) if i % 4 != 3)
    bmp.save(directory / "image.png")
    # Encode linear RGB for ordinary sRGB displays after validating the bytes.
    display_table = []
    for value in range(256):
        linear = value / 255
        display = (
            12.92 * linear
            if linear <= 0.0031308
            else 1.055 * linear ** (1 / 2.4) - 0.055
        )
        display_table.append(int(255 * display + 0.5))
    bmp.point(display_table * 3).save(directory / "preview.png")
    native = directory / "native"
    native.mkdir(exist_ok=True)
    started = time.perf_counter()
    subprocess.run(
        [str(ROOT / "util/rt_reference/out/native"), *map(str, case)],
        cwd=native,
        check=True,
    )
    native_seconds = time.perf_counter() - started

    reference = (native / "image.rgba").read_bytes()
    differences = [abs(a - b) for a, b in zip(rgba, reference)]
    # Different libm implementations may move rare paths across a mesh edge.
    assert sum(differences) / len(differences) < 0.2
    assert sum(d > 1 for d in differences) / len(differences) < 0.01
    started = time.perf_counter()
    analytic_error = analytic_smoke(scene, rgba) if case[0] == 1 else None
    analytic_seconds = time.perf_counter() - started if case[0] == 1 else None
    result = {
        "parameters": case,
        "gem5_host_seconds": host_seconds[0],
        "native_render_seconds": native_seconds,
        "analytic_smoke_seconds": analytic_seconds,
        "sha256": hashlib.sha256(rgba).hexdigest(),
        "native_mean_byte_error": sum(differences) / len(differences),
        "native_max_byte_error": max(differences),
        "analytic_smoke_max_byte_error": analytic_error,
        "scene_bytes": len(raw_scene),
        "job_bytes": len(raw_job),
        "idle_accelerators": 8,
        "accelerator_bus_packets": 0,
        "firmware_and_static_storage_bytes": symbols["__stack_top"]
        - 0x80000000,
    }
    (directory / "validation.json").write_text(
        json.dumps(result, indent=2) + "\n"
    )
    return result


def write_documentation(output, results, checks):
    destination = ROOT / "util/rt_reference"
    golden = destination / "golden"
    golden.mkdir(exist_ok=True)
    lines = [
        "# Verified rendering results",
        "",
        "Each golden image comes from a completed bare-metal RISC-V O3 run",
        "with eight connected, idle accelerators. The fixed scene is 96x96",
        "pixels. Golden PNGs apply the sRGB display transfer to the exported",
        "linear RGB bytes; the firmware BMP and RGBA remain linear.",
        "",
        "The smoke image shows deterministic direct lighting and shadows.",
        "Path tracing samples emitter hits without next-event estimation;",
        "one to four samples deliberately produce noisy images. With depth",
        "zero, only directly visible emissive surfaces contribute light.",
        "",
        "O3 wall time is gem5's `hostSeconds` from each `stats.txt`, measured",
        "from the simulator's statistics timer through the final dump. It is",
        "real elapsed time, not guest simulated time. These runs used up to",
        "three concurrent gem5 processes, so the numbers include contention.",
        "Native and supporting-check times use Python's monotonic clock.",
        "",
        "| Test | Mode | Samples | Secondary depth | Seed | O3 wall (s) | "
        "Native wall (s) |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    for name, result in results.items():
        mode, samples, depth, seed = result["parameters"]
        mode = "smoke" if mode else "path"
        lines.append(
            f"| {name} | {mode} | {samples} | {depth} | {seed} | "
            f"{result['gem5_host_seconds']:.2f} | "
            f"{result['native_render_seconds']:.6f} |"
        )
        Image.open(output / name / "preview.png").save(golden / f"{name}.png")
    pending = [name for name in CASES if name not in results]
    if pending:
        lines += ["", "Still running: " + ", ".join(pending) + "."]
    lines += [
        "",
        "All Scene and Job records passed validation. Every run has zero",
        "accelerator bus packets. The firmware and all statically reserved",
        f"storage occupy "
        f"{results['smoke']['firmware_and_static_storage_bytes']:,} bytes, "
        "including the 16 KiB stack, within",
        "the 256 KiB budget. Native comparisons use the numerical tolerance",
        "described in [README.md](README.md).",
        "",
        "The `primary`, `depth1`, and `path1` cases isolate depth changes.",
        "`path1` and `path4` isolate sample count. `path4` and `seed2` isolate",
        "seed. `repeat` must equal `path4` byte for byte; `depth32` checks the",
        "maximum supported depth and unsigned 64-bit seed.",
        "",
        "## Supporting checks",
        "",
        "These checks do not export images. The independent smoke reference",
        f"has a maximum RGB byte error of "
        f"{results['smoke']['analytic_smoke_max_byte_error']}.",
        "",
        "| Check | Wall time (s) |",
        "| --- | ---: |",
    ]
    for check in checks:
        lines.append(f"| {check['name']} | {check['seconds']:.6f} |")
    for name, result in results.items():
        lines += [
            "",
            f"## {name}",
            "",
            f"![{name} golden image](golden/{name}.png)",
            "",
            "Raw linear RGBA SHA-256:",
            "",
            f"`{result['sha256']}`",
            "",
            f"Native mean / maximum byte error: "
            f"{result['native_mean_byte_error']:.6f} / "
            f"{result['native_max_byte_error']}.",
        ]
    lines += [
        "",
        "## Reproduce",
        "",
        "```sh",
        "make -C util/rt_reference check out/native",
        "python3 util/rt_reference/validate.py --jobs=3",
        "```",
        "",
        "Add `--inspect-only` to recheck completed output directories.",
        "`--write-documentation` explicitly replaces this report and the",
        "golden images after the entire matrix passes. Complete measurements",
        "are stored in [golden/manifest.json](golden/manifest.json).",
    ]
    (destination / "RESULTS.md").write_text("\n".join(lines) + "\n")
    (golden / "manifest.json").write_text(
        json.dumps({"renders": results, "checks": checks}, indent=2) + "\n"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--gem5", type=Path, default=ROOT / "build/RISCV/gem5.opt"
    )
    parser.add_argument(
        "--output", type=Path, default=ROOT / "m5out/rt_reference/validated"
    )
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--inspect-only", action="store_true")
    parser.add_argument("--write-documentation", action="store_true")
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    symbols = {}
    nm = subprocess.check_output(
        [
            "riscv64-unknown-elf-nm",
            str(ROOT / "util/rt_reference/out/host.elf"),
        ],
        text=True,
    )
    for line in nm.splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    assert symbols["__stack_top"] - 0x80000000 <= 262144
    assert not any(name in symbols for name in ("malloc", "calloc", "realloc"))

    invalid_options = [
        ["--render_mode=2"],
        ["--samples_per_pixel=0"],
        ["--samples_per_pixel=4097"],
        ["--max_secondary_depth=-1"],
        ["--max_secondary_depth=33"],
        ["--seed=-1"],
        ["--seed=18446744073709551616"],
        ["--render_mode=1", "--samples_per_pixel=2"],
        ["--render_mode=1", "--max_secondary_depth=1"],
    ]
    checks = []
    for index, options in enumerate(invalid_options):
        directory = args.output / f"invalid-{index}"
        directory.mkdir(exist_ok=True)
        command = [
            str(args.gem5.resolve()),
            f"--outdir={directory}",
            str(ROOT / "configs/example/rt_accel.py"),
            "--backend=reference",
            *options,
        ]
        started = time.perf_counter()
        with (directory / "run.log").open("w") as log:
            result = subprocess.run(
                command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT
            )
        assert result.returncode == 2
        assert "error:" in (directory / "run.log").read_text()
        checks.append(
            {
                "name": "Reject " + " ".join(options),
                "seconds": time.perf_counter() - started,
            }
        )

    def run(item):
        name, case = item
        directory = args.output / name
        directory.mkdir(exist_ok=True)
        if not args.inspect_only:
            command = [
                str(args.gem5.resolve()),
                f"--outdir={directory}",
                str(ROOT / "configs/example/rt_accel.py"),
                "--backend=reference",
                f"--render-mode={case[0]}",
                f"--samples-per-pixel={case[1]}",
                f"--max-secondary-depth={case[2]}",
                f"--seed={case[3]}",
            ]
            print(f"Starting {name}: {case}", flush=True)
            with (directory / "run.log").open("w") as log:
                subprocess.run(
                    command,
                    cwd=ROOT,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    check=True,
                )
        result = inspect(directory, case, symbols)
        print(f"PASS {name}: {result}", flush=True)
        return name, result

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = dict(pool.map(run, CASES.items()))
    assert results["path4"]["sha256"] == results["repeat"]["sha256"]
    assert results["path4"]["sha256"] != results["seed2"]["sha256"]
    assert results["path4"]["sha256"] != results["path1"]["sha256"]
    assert results["primary"]["sha256"] != results["depth1"]["sha256"]
    assert results["depth1"]["sha256"] != results["path4"]["sha256"]
    started = time.perf_counter()
    subprocess.run(
        [
            str(ROOT / "util/rt_reference/out/test"),
            str(args.output / "smoke/scene.bin"),
        ],
        check=True,
    )
    checks.append(
        {
            "name": "C suite including full-scene traversal and tiling",
            "seconds": time.perf_counter() - started,
        }
    )
    checks.append(
        {
            "name": "Independent double-precision smoke lighting (64 pixels)",
            "seconds": results["smoke"]["analytic_smoke_seconds"],
        }
    )
    (args.output / "validation.json").write_text(
        json.dumps(results, indent=2) + "\n"
    )
    comparison = Image.new("RGB", (4 * 240, 2 * 260), "#eeeeee")
    draw = ImageDraw.Draw(comparison)
    for index, (name, case) in enumerate(CASES.items()):
        x, y = (index % 4) * 240, (index // 4) * 260
        image = Image.open(args.output / name / "preview.png")
        comparison.paste(
            image.resize((192, 192), Image.Resampling.NEAREST),
            (x + 24, y + 12),
        )
        draw.text((x + 12, y + 212), name, fill="black")
        draw.text(
            (x + 12, y + 228),
            f"mode={case[0]} spp={case[1]} depth={case[2]}",
            fill="black",
        )
    comparison.save(args.output / "comparison.png")
    (args.output / "checks.json").write_text(
        json.dumps(checks, indent=2) + "\n"
    )
    if args.write_documentation:
        write_documentation(args.output, results, checks)
    print(f"All validation passed. Images: {args.output}", flush=True)


if __name__ == "__main__":
    main()
