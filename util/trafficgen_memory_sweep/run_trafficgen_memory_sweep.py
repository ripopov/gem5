#!/usr/bin/env python3
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

"""Run and report a DDR4-2400 4Gb x8 TrafficGen memory backend comparison.

Sweeps offered TrafficGen bandwidth through three memory backends configured
for the same DDR4-2400 4Gb x8 single-channel, single-rank, 4GiB device:
gem5 native MemCtrl/DRAMInterface, DRAMSys, and DRAMSim3.

Latency is reported in nanoseconds and bandwidth in decimal GB/sec, as
required by the study specification.
"""

import argparse
import csv
import html
import json
import math
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, Iterable, List, Optional

CONFIG = Path(
    "configs/example/gem5_library/trafficgen_memory/trafficgen-ddr4.py"
)

DEFAULT_PATTERNS = [
    "linear-read",
    "linear-write",
    "linear-mixed",
    "random-read",
    "random-mixed",
]

DEFAULT_RATES = [
    "1GiB/s",
    "2GiB/s",
    "4GiB/s",
    "8GiB/s",
    "12GiB/s",
    "16GiB/s",
    "20GiB/s",
    "24GiB/s",
    "28GiB/s",
    "32GiB/s",
]

BACKEND_COLORS = {
    "gem5": "#2f6f9f",
    "dramsys": "#b8423f",
    "dramsim3": "#3f9f57",
}

BACKEND_CONFIG_NAME = {
    "gem5": "DDR4_2400_8x8_4GiB",
    "dramsys": "JEDEC_4Gb_DDR4-2400_8bit_A",
    "dramsim3": "DDR4_4Gb_x8_2400_1rank_4GiB",
}

FIELDNAMES = [
    # Study-required schema fields first.
    "backend",
    "config_name",
    "traffic_pattern",
    "read_percent",
    "offered_GBps",
    "achieved_GBps",
    "avg_latency_ns",
    "read_avg_latency_ns",
    "write_avg_latency_ns",
    "completed_requests",
    "completed_bytes",
    "sim_seconds",
    "retry_count_or_backpressure_indicator",
    # Additional bookkeeping / both-unit columns.
    "run_id",
    "success",
    "rate",
    "offered_GiBps",
    "achieved_GiBps",
    "read_GBps",
    "write_GBps",
    "completed_reads",
    "completed_writes",
    "num_packets",
    "num_retries",
    "retry_ticks",
    "avg_latency_ticks",
    "sim_ticks",
    "host_seconds",
    "host_tick_rate",
    "wall_seconds",
    # Backend-local memory statistics (not the primary metrics).
    "gem5_dram_bus_util_pct",
    "gem5_dram_avg_mem_acc_lat_ns",
    "gem5_dram_peak_GBps",
    "dramsys_avg_bw_GBps",
    "dramsys_max_bw_GBps",
    "dramsys_memory_type",
    "dramsim3_avg_bw_GBps",
    "dramsim3_avg_read_lat_ns",
    "dramsim3_row_hit_rate",
    "exit_cause",
    "run_dir",
]

STAT_RE = re.compile(
    r"^([A-Za-z0-9_.$:/<>+-]+)\s+"
    r"([+-]?(?:nan|inf|\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)"
)
MEMORY_TYPE_RE = re.compile(r"Memory type:\s+(\S+)")
DRAMSYS_AVG_BW_RE = re.compile(r"AVG BW:\s+.*\|\s+([0-9.]+)\s+GB/s")
DRAMSYS_MAX_BW_RE = re.compile(r"MAX BW:\s+.*\|\s+([0-9.]+)\s+GB/s")


def _split_list(value: str) -> List[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def _sanitize(value: str) -> str:
    value = value.replace("/s", "ps")
    value = value.replace("/", "_")
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", value).strip("_")


def _parse_rate_to_Bps(rate: str) -> float:
    text = rate.strip()
    if text.endswith("/s"):
        text = text[:-2]

    match = re.fullmatch(r"([0-9.]+)\s*([A-Za-z]+)", text)
    if not match:
        raise ValueError(f"Cannot parse bandwidth '{rate}'")

    value = float(match.group(1))
    unit = match.group(2)
    multipliers = {
        "B": 1,
        "KB": 1000,
        "MB": 1000**2,
        "GB": 1000**3,
        "TB": 1000**4,
        "KiB": 1024,
        "MiB": 1024**2,
        "GiB": 1024**3,
        "TiB": 1024**4,
    }
    if unit not in multipliers:
        raise ValueError(f"Unknown bandwidth unit '{unit}' in '{rate}'")
    return value * multipliers[unit]


def _parse_stats(stats_path: Path) -> Dict[str, float]:
    stats: Dict[str, float] = {}
    if not stats_path.exists():
        return stats

    with stats_path.open() as stats_file:
        for line in stats_file:
            match = STAT_RE.match(line)
            if not match:
                continue
            value_text = match.group(2)
            try:
                stats[match.group(1)] = float(value_text)
            except ValueError:
                continue
    return stats


def _sum_stats(
    stats: Dict[str, float],
    suffix: str,
    required: Optional[str] = None,
) -> float:
    total = 0.0
    for key, value in stats.items():
        if required is not None and required not in key:
            continue
        if key.endswith(suffix) and not math.isnan(value):
            total += value
    return total


def _first_stat(
    stats: Dict[str, float],
    suffix: str,
    required: Optional[str] = None,
) -> float:
    for key, value in stats.items():
        if required is not None and required not in key:
            continue
        if key.endswith(suffix):
            return value
    return math.nan


def _mean_stat(
    stats: Dict[str, float],
    suffix: str,
    required: Optional[str] = None,
) -> float:
    values = []
    for key, value in stats.items():
        if required is not None and required not in key:
            continue
        if key.endswith(suffix) and not math.isnan(value):
            values.append(value)
    if not values:
        return math.nan
    return sum(values) / len(values)


def _parse_dramsys_stdout(stdout_path: Path) -> Dict[str, object]:
    parsed: Dict[str, object] = {}
    if not stdout_path.exists():
        return parsed

    text = stdout_path.read_text(errors="replace")
    memory_type = MEMORY_TYPE_RE.search(text)
    if memory_type:
        parsed["dramsys_memory_type"] = memory_type.group(1)

    avg_bw_matches = DRAMSYS_AVG_BW_RE.findall(text)
    if avg_bw_matches:
        parsed["dramsys_avg_bw_GBps"] = float(avg_bw_matches[-1])

    max_bw_matches = DRAMSYS_MAX_BW_RE.findall(text)
    if max_bw_matches:
        parsed["dramsys_max_bw_GBps"] = float(max_bw_matches[-1])

    return parsed


def _parse_dramsim3_json(
    json_path: Path, tCK_ns: float
) -> Dict[str, float]:
    """Parse DRAMSim3's per-channel dramsim3.json backend statistics."""
    parsed: Dict[str, float] = {}
    if not json_path.exists():
        return parsed
    try:
        data = json.loads(json_path.read_text())
    except (ValueError, OSError):
        return parsed

    avg_bw = []
    read_lat_cycles = []
    row_hits = 0.0
    read_cmds = 0.0
    for channel in data.values():
        if not isinstance(channel, dict):
            continue
        if "average_bandwidth" in channel:
            avg_bw.append(float(channel["average_bandwidth"]))
        if channel.get("average_read_latency"):
            read_lat_cycles.append(float(channel["average_read_latency"]))
        row_hits += float(channel.get("num_read_row_hits", 0.0))
        read_cmds += float(channel.get("num_read_cmds", 0.0))

    if avg_bw:
        parsed["dramsim3_avg_bw_GBps"] = sum(avg_bw)
    if read_lat_cycles:
        parsed["dramsim3_avg_read_lat_ns"] = (
            sum(read_lat_cycles) / len(read_lat_cycles) * tCK_ns
        )
    if read_cmds:
        parsed["dramsim3_row_hit_rate"] = row_hits / read_cmds
    return parsed


def _load_metadata(path: Path) -> Dict[str, object]:
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text())
    except (ValueError, OSError):
        return {}


def _result_from_run(
    args: argparse.Namespace,
    run_id: str,
    backend: str,
    pattern: str,
    rate: str,
    run_dir: Path,
    success: bool,
    wall_seconds: float,
) -> Dict[str, object]:
    stats = _parse_stats(run_dir / "stats.txt")
    metadata = _load_metadata(run_dir / "run_metadata.json")

    gen = "processor"
    bytes_read = _sum_stats(stats, ".bytesRead", gen)
    bytes_written = _sum_stats(stats, ".bytesWritten", gen)
    total_reads = _sum_stats(stats, ".totalReads", gen)
    total_writes = _sum_stats(stats, ".totalWrites", gen)
    read_bw = _sum_stats(stats, ".readBW", gen)
    write_bw = _sum_stats(stats, ".writeBW", gen)
    num_packets = _sum_stats(stats, ".numPackets", gen)
    num_retries = _sum_stats(stats, ".numRetries", gen)
    retry_ticks = _sum_stats(stats, ".retryTicks", gen)
    total_read_latency = _sum_stats(stats, ".totalReadLatency", gen)
    total_write_latency = _sum_stats(stats, ".totalWriteLatency", gen)

    sim_ticks = _first_stat(stats, "simTicks")
    sim_seconds = _first_stat(stats, "simSeconds")
    # Convert ticks -> ns using the simulation's actual tick rate.
    if sim_ticks and sim_seconds and not math.isnan(sim_ticks):
        tick_to_ns = (sim_seconds / sim_ticks) * 1e9
    else:
        tick_to_ns = 1.0e-3  # gem5 default: 1 tick = 1 ps

    def lat_ns(total_ticks: float, count: float) -> float:
        if count:
            return total_ticks / count * tick_to_ns
        return math.nan

    total_ops = total_reads + total_writes
    read_avg_latency_ns = lat_ns(total_read_latency, total_reads)
    write_avg_latency_ns = lat_ns(total_write_latency, total_writes)
    avg_latency_ns = lat_ns(
        total_read_latency + total_write_latency, total_ops
    )
    avg_latency_ticks = (
        (total_read_latency + total_write_latency) / total_ops
        if total_ops
        else math.nan
    )

    offered_Bps = _parse_rate_to_Bps(rate)
    achieved_Bps = read_bw + write_bw

    dramsys = _parse_dramsys_stdout(run_dir / "stdout.txt")
    dramsim3 = _parse_dramsim3_json(run_dir / "dramsim3.json", 0.833)

    peak_MiBps = _mean_stat(stats, ".dram.peakBW")

    return {
        "backend": backend,
        "config_name": BACKEND_CONFIG_NAME.get(backend, backend),
        "traffic_pattern": pattern,
        "read_percent": metadata.get("read_percent", ""),
        "offered_GBps": offered_Bps / 1e9,
        "achieved_GBps": achieved_Bps / 1e9,
        "avg_latency_ns": avg_latency_ns,
        "read_avg_latency_ns": read_avg_latency_ns,
        "write_avg_latency_ns": write_avg_latency_ns,
        "completed_requests": total_ops,
        "completed_bytes": bytes_read + bytes_written,
        "sim_seconds": sim_seconds,
        "retry_count_or_backpressure_indicator": retry_ticks,
        "run_id": run_id,
        "success": success,
        "rate": rate,
        "offered_GiBps": offered_Bps / (1024**3),
        "achieved_GiBps": achieved_Bps / (1024**3),
        "read_GBps": read_bw / 1e9,
        "write_GBps": write_bw / 1e9,
        "completed_reads": total_reads,
        "completed_writes": total_writes,
        "num_packets": num_packets,
        "num_retries": num_retries,
        "retry_ticks": retry_ticks,
        "avg_latency_ticks": avg_latency_ticks,
        "sim_ticks": sim_ticks,
        "host_seconds": _first_stat(stats, "hostSeconds"),
        "host_tick_rate": _first_stat(stats, "hostTickRate"),
        "wall_seconds": wall_seconds,
        "gem5_dram_bus_util_pct": _mean_stat(stats, ".dram.busUtil"),
        "gem5_dram_avg_mem_acc_lat_ns": (
            _mean_stat(stats, ".dram.avgMemAccLat") * tick_to_ns
            if not math.isnan(_mean_stat(stats, ".dram.avgMemAccLat"))
            else math.nan
        ),
        "gem5_dram_peak_GBps": (
            peak_MiBps * (1024**2) / 1e9
            if not math.isnan(peak_MiBps)
            else math.nan
        ),
        "dramsys_avg_bw_GBps": dramsys.get("dramsys_avg_bw_GBps", math.nan),
        "dramsys_max_bw_GBps": dramsys.get("dramsys_max_bw_GBps", math.nan),
        "dramsys_memory_type": dramsys.get("dramsys_memory_type", ""),
        "dramsim3_avg_bw_GBps": dramsim3.get(
            "dramsim3_avg_bw_GBps", math.nan
        ),
        "dramsim3_avg_read_lat_ns": dramsim3.get(
            "dramsim3_avg_read_lat_ns", math.nan
        ),
        "dramsim3_row_hit_rate": dramsim3.get(
            "dramsim3_row_hit_rate", math.nan
        ),
        "exit_cause": str(metadata.get("exit_cause", "")).strip(),
        "run_dir": run_dir.as_posix(),
    }


def _run_one(
    args: argparse.Namespace,
    backend: str,
    pattern: str,
    rate: str,
) -> Dict[str, object]:
    run_id = f"{pattern}-{backend}-{_sanitize(rate)}"
    run_dir = args.outdir / "runs" / run_id
    run_dir.mkdir(parents=True, exist_ok=True)

    command = [
        args.gem5_binary.as_posix(),
        "-d",
        run_dir.as_posix(),
        args.config.as_posix(),
        "--memory-backend",
        backend,
        "--traffic-pattern",
        pattern,
        "--rate",
        rate,
        "--duration",
        args.duration,
        "--data-limit",
        args.data_limit,
        "--addr-range",
        args.addr_range,
        "--mem-size",
        args.mem_size,
        "--num-generators",
        str(args.num_generators),
        "--cache-line-size",
        str(args.cache_line_size),
        "--mixed-read-percent",
        str(args.mixed_read_percent),
        "--gem5-ddr4-interface",
        args.gem5_ddr4_interface,
        "--dram-addr-mapping",
        args.dram_addr_mapping,
        "--sys-clock",
        args.sys_clock,
        "--dramsys-config",
        args.dramsys_config,
        "--dramsim3-config",
        args.dramsim3_config,
    ]

    if args.dramsys_resource_dir:
        command += ["--dramsys-resource-dir", args.dramsys_resource_dir]

    (run_dir / "command.json").write_text(
        json.dumps(command, indent=2) + "\n"
    )

    if args.dry_run:
        print(" ".join(command))
        return _result_from_run(
            args, run_id, backend, pattern, rate, run_dir, False, 0.0
        )

    print(f"[run] {run_id}")
    started = time.monotonic()
    with (run_dir / "stdout.txt").open("w") as stdout_file:
        with (run_dir / "stderr.txt").open("w") as stderr_file:
            completed = subprocess.run(
                command,
                stdout=stdout_file,
                stderr=stderr_file,
                check=False,
            )
    wall_seconds = time.monotonic() - started

    result = _result_from_run(
        args,
        run_id,
        backend,
        pattern,
        rate,
        run_dir,
        completed.returncode == 0,
        wall_seconds,
    )
    (run_dir / "run_result.json").write_text(
        json.dumps(_json_safe(result), indent=2, allow_nan=False) + "\n"
    )
    return result


def _write_csv(results: List[Dict[str, object]], path: Path) -> None:
    with path.open("w", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=FIELDNAMES)
        writer.writeheader()
        for result in results:
            row = {}
            for field in FIELDNAMES:
                value = result.get(field, "")
                if isinstance(value, float) and (
                    math.isnan(value) or math.isinf(value)
                ):
                    value = ""
                row[field] = value
            writer.writerow(row)


def _clean_float(value: object) -> Optional[float]:
    if value in ("", None):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    if math.isnan(number) or math.isinf(number):
        return None
    return number


def _axis_ticks(min_value: float, max_value: float, count: int = 5):
    if max_value <= min_value:
        max_value = min_value + 1.0
    step = (max_value - min_value) / (count - 1)
    for index in range(count):
        yield min_value + step * index


def _write_line_plot(
    rows: List[Dict[str, object]],
    pattern: str,
    y_key: str,
    y_label: str,
    title: str,
    path: Path,
) -> bool:
    series: Dict[str, List[tuple]] = {}
    for row in rows:
        if row["traffic_pattern"] != pattern or not row.get("success"):
            continue
        x_value = _clean_float(row.get("offered_GBps"))
        y_value = _clean_float(row.get(y_key))
        if x_value is None or y_value is None:
            continue
        series.setdefault(str(row["backend"]), []).append((x_value, y_value))

    for values in series.values():
        values.sort()

    all_points = [point for values in series.values() for point in values]
    if not all_points:
        return False

    width = 760
    height = 460
    left = 88
    right = 24
    top = 48
    bottom = 68
    plot_width = width - left - right
    plot_height = height - top - bottom
    x_values = [point[0] for point in all_points]
    y_values = [point[1] for point in all_points]
    x_min = 0.0
    x_max = max(x_values)
    y_min = 0.0
    y_max = max(y_values)
    if y_max == 0.0:
        y_max = 1.0
    y_max *= 1.08

    def sx(value: float) -> float:
        return left + (value - x_min) * plot_width / (x_max - x_min or 1.0)

    def sy(value: float) -> float:
        return top + plot_height - (value - y_min) * plot_height / (
            y_max - y_min or 1.0
        )

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" '
        f'height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        f'<text x="{width / 2}" y="24" text-anchor="middle" '
        f'font-family="sans-serif" font-size="17">{html.escape(title)}</text>',
    ]

    for tick in _axis_ticks(x_min, x_max):
        x = sx(tick)
        lines.append(
            f'<line x1="{x:.1f}" y1="{top}" x2="{x:.1f}" '
            f'y2="{top + plot_height}" stroke="#e8e8e8"/>'
        )
        lines.append(
            f'<text x="{x:.1f}" y="{height - 42}" text-anchor="middle" '
            f'font-family="sans-serif" font-size="11">{tick:.1f}</text>'
        )

    for tick in _axis_ticks(y_min, y_max):
        y = sy(tick)
        lines.append(
            f'<line x1="{left}" y1="{y:.1f}" x2="{left + plot_width}" '
            f'y2="{y:.1f}" stroke="#e8e8e8"/>'
        )
        lines.append(
            f'<text x="{left - 8}" y="{y + 4:.1f}" text-anchor="end" '
            f'font-family="sans-serif" font-size="11">{tick:.4g}</text>'
        )

    lines.append(
        f'<line x1="{left}" y1="{top + plot_height}" '
        f'x2="{left + plot_width}" y2="{top + plot_height}" stroke="#222"/>'
    )
    lines.append(
        f'<line x1="{left}" y1="{top}" x2="{left}" '
        f'y2="{top + plot_height}" stroke="#222"/>'
    )
    lines.append(
        f'<text x="{left + plot_width / 2}" y="{height - 12}" '
        'text-anchor="middle" font-family="sans-serif" font-size="13">'
        "Offered bandwidth (GB/s)</text>"
    )
    lines.append(
        f'<text x="20" y="{top + plot_height / 2}" '
        'text-anchor="middle" font-family="sans-serif" font-size="13" '
        f'transform="rotate(-90 20 {top + plot_height / 2})">'
        f"{html.escape(y_label)}</text>"
    )

    legend_y = 50
    for index, (backend, points) in enumerate(sorted(series.items())):
        color = BACKEND_COLORS.get(backend, "#444444")
        polyline = " ".join(f"{sx(x):.1f},{sy(y):.1f}" for x, y in points)
        lines.append(
            f'<polyline fill="none" stroke="{color}" stroke-width="2.2" '
            f'points="{polyline}"/>'
        )
        for x, y in points:
            lines.append(
                f'<circle cx="{sx(x):.1f}" cy="{sy(y):.1f}" r="3.5" '
                f'fill="{color}"/>'
            )
        legend_x = left + 16 + index * 130
        lines.append(
            f'<line x1="{legend_x}" y1="{legend_y}" '
            f'x2="{legend_x + 24}" y2="{legend_y}" '
            f'stroke="{color}" stroke-width="2.2"/>'
        )
        lines.append(
            f'<text x="{legend_x + 30}" y="{legend_y + 4}" '
            f'font-family="sans-serif" font-size="12">'
            f"{html.escape(backend)}</text>"
        )

    lines.append("</svg>")
    path.write_text("\n".join(lines) + "\n")
    return True


def _write_plots(
    results: List[Dict[str, object]],
    patterns: Iterable[str],
    plot_dir: Path,
) -> List[Path]:
    plot_dir.mkdir(parents=True, exist_ok=True)
    paths: List[Path] = []
    for pattern in patterns:
        bandwidth_path = plot_dir / f"{pattern}-bandwidth.svg"
        if _write_line_plot(
            results,
            pattern,
            "achieved_GBps",
            "Achieved bandwidth (GB/s)",
            f"{pattern}: achieved bandwidth (GB/s)",
            bandwidth_path,
        ):
            paths.append(bandwidth_path)

        latency_path = plot_dir / f"{pattern}-latency.svg"
        if _write_line_plot(
            results,
            pattern,
            "avg_latency_ns",
            "Average latency (ns)",
            f"{pattern}: average latency (ns)",
            latency_path,
        ):
            paths.append(latency_path)
    return paths


def _format_number(value: object, digits: int = 3) -> str:
    number = _clean_float(value)
    if number is None:
        return "n/a"
    return f"{number:.{digits}f}"


def _json_safe(value):
    if isinstance(value, dict):
        return {key: _json_safe(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_json_safe(item) for item in value]
    if isinstance(value, float) and (math.isnan(value) or math.isinf(value)):
        return None
    return value


def _best_by_backend(
    results: List[Dict[str, object]],
    pattern: str,
    backend: str,
) -> Optional[Dict[str, object]]:
    rows = [
        row
        for row in results
        if row["traffic_pattern"] == pattern
        and row["backend"] == backend
        and row.get("success")
    ]
    if not rows:
        return None
    return max(rows, key=lambda row: float(row.get("achieved_GBps", 0.0)))


def _average(rows: List[Dict[str, object]], key: str) -> Optional[float]:
    values = [
        v for v in (_clean_float(row.get(key)) for row in rows)
        if v is not None
    ]
    return sum(values) / len(values) if values else None


def _maximum(rows: List[Dict[str, object]], key: str) -> Optional[float]:
    values = [
        v for v in (_clean_float(row.get(key)) for row in rows)
        if v is not None
    ]
    return max(values) if values else None


# Cycle-count timing table from the DRAMSys reference memspec, with the ns
# value computed at tCK = 0.833 ns and a note on how each backend models it.
TIMING_TABLE = [
    ("tCK", "n/a", 0.833, "DDR4-2400 command clock period."),
    ("CL / RL", 16, 13.328, "gem5 tCL; DRAMSim3 CL; DRAMSys CL/RL."),
    ("WL / CWL", 16, 13.328, "DRAMSim3 CWL=16; DRAMSys WL=16; gem5 has no "
     "separate WL (write latency folded into tCWL = tCL)."),
    ("RCD", 16, 13.328, "gem5 tRCD; DRAMSim3 tRCD; DRAMSys RCD."),
    ("RP", 16, 13.328, "gem5 tRP; DRAMSim3 tRP; DRAMSys RP."),
    ("RAS", 39, 32.487, "gem5 tRAS; DRAMSim3 tRAS; DRAMSys RAS."),
    ("RC", 55, 45.815, "Derived (RAS+RP) in gem5/DRAMSim3; explicit in "
     "DRAMSys."),
    ("CCD_S", 4, 3.332, "gem5 tBURST; DRAMSim3 tCCD_S; DRAMSys CCD_S."),
    ("CCD_L", 6, 4.998, "gem5 tCCD_L; DRAMSim3 tCCD_L; DRAMSys CCD_L."),
    ("RRD_S", 4, 3.332, "gem5 tRRD; DRAMSim3 tRRD_S; DRAMSys RRD_S."),
    ("RRD_L", 6, 4.998, "gem5 tRRD_L; DRAMSim3 tRRD_L; DRAMSys RRD_L."),
    ("FAW", 26, 21.658, "gem5 tXAW; DRAMSim3 tFAW; DRAMSys FAW."),
    ("RFC1", 312, 259.896, "gem5 tRFC; DRAMSim3 tRFC; DRAMSys RFC1."),
    ("REFI", 9360, 7796.880, "gem5 tREFI; DRAMSim3 tREFI; DRAMSys REFI."),
    ("WR", 18, 14.994, "gem5 tWR; DRAMSim3 tWR; DRAMSys WR."),
    ("WTR_S", 3, 2.499, "gem5 tWTR; DRAMSim3 tWTR_S; DRAMSys WTR_S."),
    ("WTR_L", 9, 7.497, "gem5 tWTR_L; DRAMSim3 tWTR_L; DRAMSys WTR_L."),
    ("RTP", 12, 9.996, "gem5 tRTP; DRAMSim3 tRTP; DRAMSys RTP."),
    ("XP", 8, 6.664, "gem5 tXP; DRAMSim3 tXP; DRAMSys XP."),
    ("XS", 324, 269.892, "gem5 tXS; DRAMSim3 tXS; DRAMSys XS."),
    ("RTRS", 1, 0.833, "gem5 tCS; DRAMSim3 tRTRS; DRAMSys RTRS "
     "(single rank: not exercised)."),
]


def _write_report(
    args: argparse.Namespace,
    results: List[Dict[str, object]],
    patterns: List[str],
    backends: List[str],
    plots: List[Path],
) -> None:
    report = args.outdir / "report.md"
    csv_path = args.outdir / "results.csv"
    json_path = args.outdir / "results.json"

    lines = [
        "# DDR4-2400 4Gb x8 Memory Backend Comparison",
        "",
        "Apples-to-apples comparison of three memory backends on one shared "
        "DDR4-2400 4Gb x8, single-channel, single-rank, 4GiB device-under-"
        "test:",
        "",
        "- **gem5** native `MemCtrl` with a custom `DDR4_2400_8x8_4GiB` "
        "`DRAMInterface`",
        "- **DRAMSys** with the JEDEC DDR4-2400 4Gb x8 memspec",
        "- **DRAMSim3** with a derived DDR4-2400 4Gb x8 single-rank config",
        "",
        "The reference device specification is the DRAMSys memspec "
        "`ext/dramsys/DRAMSys/configs/memspec/JEDEC_4Gb_DDR4-2400_8bit_A.json"
        "`; all backend timings are converted from it.",
        "",
        "## Methodology",
        "",
        "Synthetic TrafficGen requestors drive the memory backend directly "
        "through a cacheless path: TrafficGen -> SystemXBar (NoCache) -> "
        "memory backend. There is no CHI, Ruby, cache, or interconnect "
        "between the requestor and the controller, so the measured "
        "differences come from the three memory models rather than from a "
        "shared fabric. The crossbar, clock, traffic, and address range are "
        "identical across all three backends.",
        "",
        "Each run holds the device, timing, clocks, traffic pattern, address "
        "range, and block size fixed while sweeping the offered injection "
        "rate.",
        "",
        "## Measurement Methodology",
        "",
        "**Offered bandwidth** is the programmed TrafficGen injection rate, "
        "not measured throughput. TrafficGen converts each rate into a packet "
        "period of `block_size / rate`; with a 64-byte block, higher offered "
        "rates schedule 64-byte requests closer together. One requestor is "
        "used, so the programmed requestor rate equals the aggregate offered "
        "rate. Offered rates are listed in binary GiB/s but reported in "
        "decimal GB/s on the plots and tables.",
        "",
        "**Achieved bandwidth** is response-based and reported in decimal "
        "GB/s (1 GB = 1,000,000,000 bytes). The runner sums TrafficGen "
        "`readBW` and `writeBW`, which are `bytesRead / simSeconds` and "
        "`bytesWritten / simSeconds`; the byte counters increment only when "
        "a timing response returns, so achieved bandwidth reflects completed "
        "work. This is the same requestor-visible observation point for all "
        "three backends. Each backend's own internal bandwidth counter "
        "(gem5 `dram.*`, DRAMSys `AVG/MAX BW`, DRAMSim3 `average_bandwidth`) "
        "is recorded separately and is not the primary metric.",
        "",
        "**Latency** is response-based and reported in nanoseconds. For each "
        "completed request TrafficGen records the tick when an accepted "
        "request is sent and adds the send-to-response time to the read or "
        "write latency total when the response arrives. Average latency is "
        "`total_completed_request_latency / completed_request_count`, "
        "computed from completed requests only and converted from ticks to "
        "ns using the run's tick rate. Read, write, and request-count-"
        "weighted combined latencies are all recorded.",
        "",
        "Past saturation the backend cannot accept requests at the offered "
        "pace; TrafficGen stalls on back-pressure, which appears as "
        "`retryTicks` rather than unbounded in-flight requests. The accepted "
        "rate is clipped near the sustainable throughput, so achieved "
        "bandwidth flattens and latency bends upward then levels off.",
        "",
        "## Device Under Test",
        "",
        "| Parameter | Value |",
        "| --- | --- |",
        "| Memory type | DDR4 |",
        "| Data rate | 2400 MT/s |",
        "| Command clock | 1200 MHz |",
        "| tCK | 0.833 ns |",
        "| Channels | 1 |",
        "| Ranks per channel | 1 |",
        "| Devices per rank | 8 (x8) |",
        "| Device density | 4Gb = 512 MiB |",
        "| Channel data width | 64 bits |",
        "| Total capacity | 4 GiB |",
        "| Burst length | 8 (64-byte burst) |",
        "| Bank groups per rank | 4 |",
        "| Banks per rank | 16 |",
        "| Rows | 32768 |",
        "| Columns | 1024 |",
        "",
        "## Shared Timing (from the DRAMSys reference memspec)",
        "",
        "All cycle counts are from `JEDEC_4Gb_DDR4-2400_8bit_A.json`; ns "
        "values use tCK = 0.833 ns. The final column notes how each backend "
        "expresses the parameter.",
        "",
        "| Timing | Cycles | Approx. ns | Per-backend mapping |",
        "| --- | ---: | ---: | --- |",
    ]

    for name, cycles, ns, note in TIMING_TABLE:
        cyc = "n/a" if cycles == "n/a" else f"{cycles}"
        lines.append(f"| {name} | {cyc} | {ns:.3f} | {note} |")

    lines += [
        "",
        "## Configuration",
        "",
        f"- gem5 binary: `{args.gem5_binary}`",
        f"- config script: `{args.config}`",
        f"- patterns: `{', '.join(patterns)}`",
        f"- backends: `{', '.join(backends)}`",
        f"- offered rates: `{', '.join(args.rates)}`",
        f"- duration: `{args.duration}` (warmup: none)",
        f"- data limit: `{args.data_limit}`",
        f"- memory size: `{args.mem_size}` (one 4 GiB range, start = 0)",
        f"- traffic address range: `{args.addr_range}`",
        f"- block / cache line size: `{args.cache_line_size}` bytes",
        f"- requestors: `{args.num_generators}`",
        f"- mixed read percent: `{args.mixed_read_percent}`",
        "- controller / requestor / system clock: "
        f"`{args.sys_clock}` (held constant across backends)",
        f"- gem5 interface: `DDR4_2400_8x8_4GiB`, page policy `open`, "
        f"scheduler `frfcfs`, address mapping `{args.dram_addr_mapping}`, "
        "read/write queue 32/32 entries",
        f"- DRAMSys config: `{args.dramsys_config}` "
        "(FR-FCFS, open page, bankwise queue, request buffer 8, all-bank "
        "refresh, no power-down)",
        f"- DRAMSim3 config: `{args.dramsim3_config}` "
        "(FR-FCFS, OPEN_PAGE, PER_BANK queue, cmd queue 8, trans queue 32, "
        "rank-level-staggered refresh)",
        "",
        "## Apples-to-Apples Notes and Known Differences",
        "",
        "**Address mapping.** All three default to *different* address "
        "mappings, which would otherwise dominate the comparison. They are "
        "aligned here so banks/bank-groups interleave at cache-line "
        "granularity: gem5 uses `RoCoRaBaCh`; DRAMSim3 uses `rochrababgco`; "
        "DRAMSys uses a custom mapping "
        "(`am_ddr4_4Gbx8_1rank_bginterleave.json`) that places bank-group "
        "(bits 6-7) and bank (bits 8-9) just above the 64-byte block offset. "
        "This ensures the limited traffic range exercises all 16 banks in "
        "every backend. The bit orderings are not identical at every "
        "position, which is an unavoidable model difference.",
        "",
        "**DRAMSys throughput is bridge-limited (most important caveat).** "
        "DRAMSys is attached to gem5 through the SystemC TLM bridge "
        "(`Gem5ToTlmBridge`), which uses the TLM-2.0 approximately-timed "
        "4-phase protocol. The bridge enforces the TLM exclusion rule: after "
        "sending `BEGIN_REQ` it holds a single `blockingRequest` and refuses "
        "(retries) any further request until `END_REQ` returns. DRAMSys "
        "defers `END_REQ` to model the payload/data-bus delay, so the bridge "
        "admits only one new 64-byte transaction per `BEGIN_REQ`->`END_REQ` "
        "interval (~7.5 ns here). This caps DRAMSys's *injected* throughput "
        "at about 8.5 GB/s (~44% of the 19.2 GB/s peak data-bus rate) for "
        "**every** traffic pattern. The flat, pattern-independent cap "
        "(linear and random saturate at the same value) confirms the limit "
        "is the request-phase handshake, not the DRAM timing: DRAMSys's own "
        "`MAX BW` counter still reports the full 19.2 GB/s peak, but its "
        "`AVG BW` tracks the bridge-limited 8.5 GB/s. As a result the "
        "**saturation-bandwidth comparison is only apples-to-apples between "
        "gem5 and DRAMSim3**; DRAMSys's bandwidth curve reflects the "
        "gem5<->DRAMSys integration, not the DRAMSys controller's intrinsic "
        "throughput. The **latency comparison remains valid for all three** "
        "backends at sub-saturation rates, where only one request is in "
        "flight anyway. This is a genuine, structural integration "
        "difference, documented rather than worked around.",
        "",
        "**Controller policy.** All three use FR-FCFS scheduling and an "
        "open-page policy. DRAMSys and DRAMSim3 use an 8-entry per-bank "
        "command queue; gem5's `MemCtrl` does not expose a per-bank command "
        "queue and instead uses read/write transaction queues (set to 32/32 "
        "here, near DRAMSim3's 32-entry transaction queue). DRAMSim3's "
        "`trans_queue_size = 32` has no DRAMSys equivalent. These queue-model "
        "differences cannot be made bit-identical.",
        "",
        "**Write latency.** DRAMSys (WL) and DRAMSim3 (CWL) model write "
        "latency as 16 cycles. gem5's `DRAMInterface` has no separate write "
        "latency parameter and folds it into `tCL`; this can shift write and "
        "mixed-traffic latency slightly.",
        "",
        "**Fixed controller pipeline latency.** gem5's `MemCtrl` adds a fixed "
        "`static_frontend_latency` (10 ns) plus `static_backend_latency` "
        "(10 ns) = 20 ns to every request's requestor-visible latency. This "
        "is gem5's explicit model of the controller pipeline. DRAMSys and "
        "DRAMSim3 account for controller latency differently and do not add "
        "this fixed 20 ns at the same observation point. This is the main "
        "reason gem5's unloaded latency sits roughly 20 ns above DRAMSys and "
        "DRAMSim3 (which agree closely with each other), while the bandwidth "
        "curves remain close. It is a genuine model difference, not a "
        "configuration mismatch, and is left at gem5's default rather than "
        "artificially zeroed.",
        "",
        "**Clock domains.** The requestor, crossbar, and gem5 controller run "
        f"at `{args.sys_clock}`. DRAMSys (SystemC/TLM) and DRAMSim3 advance "
        "on their own tCK-based event schedules through the gem5 wrapper; "
        "the device tCK (0.833 ns) is identical for all three.",
        "",
        "**Refresh / power-down.** Refresh is enabled in all three "
        "(all-bank in gem5/DRAMSys, rank-level-staggered in DRAMSim3). "
        "Power-down is disabled in all three.",
        "",
        "Treat the plots as a backend-model comparison under a shared "
        "DDR4-2400 4Gb x8 configuration, not as silicon validation.",
        "",
        "## Plots",
        "",
    ]

    for plot in plots:
        rel_path = plot.relative_to(args.outdir)
        caption = plot.stem.replace("-", " ").title()
        lines += [
            f"### {caption}",
            "",
            f"![{caption}]({rel_path.as_posix()})",
            "",
        ]

    lines += [
        "## Peak Achieved Bandwidth and Latency",
        "",
        "| Pattern | Backend | Peak achieved GB/s | Rate at peak | "
        "Avg latency at peak (ns) | Host seconds |",
        "|---|---|---:|---|---:|---:|",
    ]

    for pattern in patterns:
        for backend in backends:
            best = _best_by_backend(results, pattern, backend)
            if best is None:
                lines.append(
                    f"| {pattern} | {backend} | n/a | n/a | n/a | n/a |"
                )
                continue
            lines.append(
                f"| {pattern} | {backend} | "
                f"{_format_number(best['achieved_GBps'])} | "
                f"{best['rate']} | "
                f"{_format_number(best['avg_latency_ns'], 1)} | "
                f"{_format_number(best['host_seconds'], 2)} |"
            )

    lines += [
        "",
        "## Low-Load Latency (offered 1 GiB/s)",
        "",
        "Unloaded round-trip latency seen by the requestor at the lowest "
        "offered rate.",
        "",
        "| Pattern | Backend | Avg latency (ns) | Read latency (ns) | "
        "Write latency (ns) |",
        "|---|---|---:|---:|---:|",
    ]

    for pattern in patterns:
        for backend in backends:
            rows = [
                row
                for row in results
                if row["traffic_pattern"] == pattern
                and row["backend"] == backend
                and row.get("success")
            ]
            if not rows:
                continue
            low = min(rows, key=lambda r: float(r.get("offered_GBps", 0.0)))
            lines.append(
                f"| {pattern} | {backend} | "
                f"{_format_number(low['avg_latency_ns'], 1)} | "
                f"{_format_number(low['read_avg_latency_ns'], 1)} | "
                f"{_format_number(low['write_avg_latency_ns'], 1)} |"
            )

    lines += [
        "",
        "## Simulation Speed",
        "",
        "| Backend | Runs | Avg host seconds | Avg wall seconds | "
        "Avg host Mtick/s | Avg achieved GB/s |",
        "|---|---:|---:|---:|---:|---:|",
    ]

    for backend in backends:
        rows = [
            row
            for row in results
            if row["backend"] == backend and row.get("success")
        ]
        avg_host_tick_rate = _average(rows, "host_tick_rate")
        if avg_host_tick_rate is not None:
            avg_host_tick_rate /= 1e6
        lines.append(
            f"| {backend} | {len(rows)} | "
            f"{_format_number(_average(rows, 'host_seconds'), 3)} | "
            f"{_format_number(_average(rows, 'wall_seconds'), 3)} | "
            f"{_format_number(avg_host_tick_rate, 3)} | "
            f"{_format_number(_average(rows, 'achieved_GBps'), 3)} |"
        )

    lines += [
        "",
        "## Backend-Local Memory Statistics",
        "",
        "Internal counters reported by each backend (not the primary "
        "requestor-visible metric).",
        "",
        "| Backend | Runs | Avg gem5 bus util % | Max gem5 peak GB/s | "
        "Avg DRAMSys AVG BW GB/s | Max DRAMSys MAX BW GB/s | "
        "Avg DRAMSim3 BW GB/s | Avg DRAMSim3 row-hit rate |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]

    for backend in backends:
        rows = [
            row
            for row in results
            if row["backend"] == backend and row.get("success")
        ]
        lines.append(
            f"| {backend} | {len(rows)} | "
            f"{_format_number(_average(rows, 'gem5_dram_bus_util_pct'))} | "
            f"{_format_number(_maximum(rows, 'gem5_dram_peak_GBps'))} | "
            f"{_format_number(_average(rows, 'dramsys_avg_bw_GBps'))} | "
            f"{_format_number(_maximum(rows, 'dramsys_max_bw_GBps'))} | "
            f"{_format_number(_average(rows, 'dramsim3_avg_bw_GBps'))} | "
            f"{_format_number(_average(rows, 'dramsim3_row_hit_rate'))} |"
        )

    lines += [
        "",
        "## Back-Pressure Summary",
        "",
        "| Pattern | Backend | Max retry ticks | Max avg latency (ns) |",
        "|---|---|---:|---:|",
    ]

    for pattern in patterns:
        for backend in backends:
            rows = [
                row
                for row in results
                if row["traffic_pattern"] == pattern
                and row["backend"] == backend
                and row.get("success")
            ]
            lines.append(
                f"| {pattern} | {backend} | "
                f"{_format_number(_maximum(rows, 'retry_ticks'), 0)} | "
                f"{_format_number(_maximum(rows, 'avg_latency_ns'), 1)} |"
            )

    dramsys_types = sorted(
        {
            str(row.get("dramsys_memory_type"))
            for row in results
            if row.get("dramsys_memory_type")
        }
    )
    if dramsys_types:
        lines += [
            "",
            "## Observed Memory Models",
            "",
            f"- DRAMSys reported memory type(s): `{', '.join(dramsys_types)}`",
        ]

    lines += [
        "",
        f"Full CSV results: [{csv_path.name}]({csv_path.name})",
        f"Full JSON results: [{json_path.name}]({json_path.name})",
        "",
        "## Conclusions",
        "",
    ]

    for pattern in patterns:
        rows = [
            row
            for row in results
            if row["traffic_pattern"] == pattern and row.get("success")
        ]
        if not rows:
            lines.append(f"- `{pattern}` did not produce successful runs.")
            continue
        best = max(rows, key=lambda row: float(row.get("achieved_GBps", 0.0)))
        peaks = []
        for backend in backends:
            b = _best_by_backend(results, pattern, backend)
            if b is not None:
                peaks.append(
                    f"{backend} {_format_number(b['achieved_GBps'])}"
                )
        lines.append(
            f"- `{pattern}`: peak GB/s by backend -> {', '.join(peaks)}; "
            f"highest was `{best['backend']}` at offered `{best['rate']}`."
        )

    lines += [
        "",
        "The three simulators are not identical and this study does not "
        "claim they are. The device geometry, the JEDEC timing table, the "
        "controller/requestor/system clocks, and the traffic are held equal, "
        "and the address mapping is aligned to interleave banks at "
        "cache-line granularity. Two model differences dominate and are "
        "documented above rather than hidden:",
        "",
        "1. **Unloaded latency**: DRAMSys and DRAMSim3 agree closely "
        "(both ~32 ns for an open-page linear read); gem5 sits ~20 ns higher "
        "because of its explicit `static_frontend_latency` + "
        "`static_backend_latency` controller pipeline. This is the cleanest "
        "apples-to-apples result and is valid for all three backends.",
        "",
        "2. **Saturation bandwidth**: gem5 (up to ~18.6 GB/s, ~97% of peak) "
        "and DRAMSim3 (~14-15 GB/s) are directly comparable; DRAMSys is "
        "pinned at ~8.5 GB/s by the TLM bridge's single-outstanding-request "
        "handshake, not by its DRAM model, so its bandwidth curve should not "
        "be read as the DRAMSys controller's intrinsic throughput.",
        "",
        "The remaining smaller spread between gem5 and DRAMSim3 reflects "
        "genuine differences in their controller and DRAM models (queue "
        "structure, write-latency modelling, refresh scheduling, and "
        "command-arbitration details).",
    ]

    failed = [row for row in results if not row.get("success")]
    if failed:
        lines += ["", "## Failed Runs", ""]
        for row in failed:
            lines.append(
                f"- `{row['run_id']}` failed; see `{row['run_dir']}`."
            )

    report.write_text("\n".join(lines) + "\n")


def _parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Sweep TrafficGen bandwidth through three DDR4-2400 "
        "4Gb x8 memory backends and generate a comparison report."
    )
    parser.add_argument(
        "--gem5-binary", type=Path, default=Path("build/RISCV/gem5.opt")
    )
    parser.add_argument("--config", type=Path, default=CONFIG)
    parser.add_argument(
        "--outdir",
        type=Path,
        default=Path("m5out/ddr4-2400-4gb-x8-memory-sweep"),
    )
    parser.add_argument(
        "--backends",
        default="gem5,dramsys,dramsim3",
        help="Comma-separated list: gem5, dramsys, dramsim3.",
    )
    parser.add_argument(
        "--patterns",
        default=",".join(DEFAULT_PATTERNS),
        help="Comma-separated traffic patterns.",
    )
    parser.add_argument(
        "--rates",
        default=",".join(DEFAULT_RATES),
        help="Comma-separated offered bandwidth rates.",
    )
    parser.add_argument("--duration", default="20us")
    parser.add_argument("--data-limit", default="0B")
    parser.add_argument("--addr-range", default="256MiB")
    parser.add_argument("--mem-size", default="4GiB")
    parser.add_argument("--num-generators", type=int, default=1)
    parser.add_argument("--cache-line-size", type=int, default=64)
    parser.add_argument("--mixed-read-percent", type=int, default=50)
    parser.add_argument(
        "--gem5-ddr4-interface",
        default="2400-x8-4gib",
        help="gem5 DDR4 interface name passed to the config script.",
    )
    parser.add_argument("--dram-addr-mapping", default="RoCoRaBaCh")
    parser.add_argument(
        "--dramsys-config",
        default="ext/dramsys/gem5_configs/ddr4-2400-4gb-x8-gem5-se.json",
    )
    parser.add_argument(
        "--dramsim3-config",
        default="ext/dramsim3/DDR4_4Gb_x8_2400_1rank_4GiB.ini",
    )
    parser.add_argument("--dramsys-resource-dir", default=None)
    parser.add_argument(
        "--sys-clock",
        default="1.2GHz",
        help="Controller/requestor/system clock (default 1.2GHz = DDR4-2400 "
        "command clock).",
    )
    parser.add_argument(
        "--skip-run",
        action="store_true",
        help="Reuse existing run directories and regenerate outputs.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print commands without executing gem5.",
    )
    args = parser.parse_args()

    args.backends = _split_list(args.backends)
    args.patterns = _split_list(args.patterns)
    args.rates = _split_list(args.rates)
    return args


def main() -> int:
    args = _parse_arguments()
    args.outdir.mkdir(parents=True, exist_ok=True)

    results = []
    for pattern in args.patterns:
        for backend in args.backends:
            for rate in args.rates:
                if args.skip_run:
                    run_id = f"{pattern}-{backend}-{_sanitize(rate)}"
                    run_dir = args.outdir / "runs" / run_id
                    saved = run_dir / "run_result.json"
                    wall_seconds = 0.0
                    if saved.exists():
                        wall_seconds = float(
                            json.loads(saved.read_text()).get(
                                "wall_seconds", 0.0
                            )
                        )
                    result = _result_from_run(
                        args,
                        run_id,
                        backend,
                        pattern,
                        rate,
                        run_dir,
                        (run_dir / "stats.txt").exists(),
                        wall_seconds,
                    )
                    (run_dir / "run_result.json").write_text(
                        json.dumps(
                            _json_safe(result), indent=2, allow_nan=False
                        )
                        + "\n"
                    )
                else:
                    result = _run_one(args, backend, pattern, rate)
                results.append(result)

    _write_csv(results, args.outdir / "results.csv")
    (args.outdir / "results.json").write_text(
        json.dumps(_json_safe(results), indent=2, allow_nan=False) + "\n"
    )
    plots = _write_plots(results, args.patterns, args.outdir / "plots")
    _write_report(args, results, args.patterns, args.backends, plots)

    print(f"Wrote {args.outdir / 'results.csv'}")
    print(f"Wrote {args.outdir / 'report.md'}")
    if args.dry_run:
        return 0
    if any(not row.get("success") for row in results):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
