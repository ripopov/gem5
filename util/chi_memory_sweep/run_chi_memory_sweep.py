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

"""Run and report CHI RN-I synthetic memory sweeps."""

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
    "configs/example/gem5_library/chi_rni_memory/chi-rni-ddr4.py"
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

FIELDNAMES = [
    "run_id",
    "success",
    "backend",
    "pattern",
    "rate",
    "offered_Bps",
    "offered_GBps",
    "achieved_Bps",
    "achieved_GBps",
    "read_Bps",
    "write_Bps",
    "avg_latency_ticks",
    "avg_read_latency_ticks",
    "avg_write_latency_ticks",
    "bytes_read",
    "bytes_written",
    "total_reads",
    "total_writes",
    "num_packets",
    "num_retries",
    "retry_ticks",
    "request_buffer_messages",
    "request_buffer_avg_messages",
    "request_buffer_occupancy",
    "response_buffer_messages",
    "response_buffer_avg_messages",
    "sim_ticks",
    "host_seconds",
    "host_tick_rate",
    "gem5_dram_avg_queue_latency",
    "gem5_dram_avg_memory_latency",
    "gem5_dram_bus_util",
    "gem5_dram_peak_MiBps",
    "dramsys_memory_type",
    "dramsys_avg_bw_GBps",
    "dramsys_max_bw_GBps",
    "wall_seconds",
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


def _parse_stdout(stdout_path: Path) -> Dict[str, float | str]:
    parsed: Dict[str, float | str] = {}
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


def _load_metadata(path: Path) -> Dict[str, object]:
    if not path.exists():
        return {}
    return json.loads(path.read_text())


def _result_from_run(
    run_id: str,
    backend: str,
    pattern: str,
    rate: str,
    run_dir: Path,
    success: bool,
    wall_seconds: float,
) -> Dict[str, object]:
    stats = _parse_stats(run_dir / "stats.txt")
    stdout = _parse_stdout(run_dir / "stdout.txt")
    metadata = _load_metadata(run_dir / "run_metadata.json")

    required = "processor"
    bytes_read = _sum_stats(stats, ".bytesRead", required)
    bytes_written = _sum_stats(stats, ".bytesWritten", required)
    total_reads = _sum_stats(stats, ".totalReads", required)
    total_writes = _sum_stats(stats, ".totalWrites", required)
    read_bw = _sum_stats(stats, ".readBW", required)
    write_bw = _sum_stats(stats, ".writeBW", required)
    num_packets = _sum_stats(stats, ".numPackets", required)
    num_retries = _sum_stats(stats, ".numRetries", required)
    retry_ticks = _sum_stats(stats, ".retryTicks", required)
    total_read_latency = _sum_stats(stats, ".totalReadLatency", required)
    total_write_latency = _sum_stats(stats, ".totalWriteLatency", required)

    total_ops = total_reads + total_writes
    if total_ops:
        avg_latency = (
            total_read_latency + total_write_latency
        ) / total_ops
    else:
        avg_latency = math.nan

    offered_Bps = _parse_rate_to_Bps(rate)
    achieved_Bps = read_bw + write_bw

    return {
        "run_id": run_id,
        "success": success,
        "backend": backend,
        "pattern": pattern,
        "rate": rate,
        "offered_Bps": offered_Bps,
        "offered_GBps": offered_Bps / 1e9,
        "achieved_Bps": achieved_Bps,
        "achieved_GBps": achieved_Bps / 1e9,
        "read_Bps": read_bw,
        "write_Bps": write_bw,
        "avg_latency_ticks": avg_latency,
        "avg_read_latency_ticks": _mean_stat(
            stats, ".avgReadLatency", required
        ),
        "avg_write_latency_ticks": _mean_stat(
            stats, ".avgWriteLatency", required
        ),
        "bytes_read": bytes_read,
        "bytes_written": bytes_written,
        "total_reads": total_reads,
        "total_writes": total_writes,
        "num_packets": num_packets,
        "num_retries": num_retries,
        "retry_ticks": retry_ticks,
        "request_buffer_messages": _sum_stats(
            stats, ".requestToMemory.m_msg_count"
        ),
        "request_buffer_avg_messages": _mean_stat(
            stats, ".requestToMemory.m_buf_msgs"
        ),
        "request_buffer_occupancy": _mean_stat(
            stats, ".requestToMemory.m_occupancy"
        ),
        "response_buffer_messages": _sum_stats(
            stats, ".responseFromMemory.m_msg_count"
        ),
        "response_buffer_avg_messages": _mean_stat(
            stats, ".responseFromMemory.m_buf_msgs"
        ),
        "sim_ticks": _first_stat(stats, "simTicks"),
        "host_seconds": _first_stat(stats, "hostSeconds"),
        "host_tick_rate": _first_stat(stats, "hostTickRate"),
        "gem5_dram_avg_queue_latency": _mean_stat(
            stats, ".dram.avgQLat"
        ),
        "gem5_dram_avg_memory_latency": _mean_stat(
            stats, ".dram.avgMemAccLat"
        ),
        "gem5_dram_bus_util": _mean_stat(stats, ".dram.busUtil"),
        "gem5_dram_peak_MiBps": _mean_stat(stats, ".dram.peakBW"),
        "dramsys_memory_type": stdout.get("dramsys_memory_type", ""),
        "dramsys_avg_bw_GBps": stdout.get("dramsys_avg_bw_GBps", math.nan),
        "dramsys_max_bw_GBps": stdout.get("dramsys_max_bw_GBps", math.nan),
        "wall_seconds": wall_seconds,
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
        "--sys-clock",
        args.sys_clock,
        "--dramsys-config",
        args.dramsys_config,
    ]

    if args.dram_addr_mapping:
        command += ["--dram-addr-mapping", args.dram_addr_mapping]
    if args.dramsys_resource_dir:
        command += ["--dramsys-resource-dir", args.dramsys_resource_dir]

    (run_dir / "command.json").write_text(
        json.dumps(command, indent=2) + "\n"
    )

    if args.dry_run:
        print(" ".join(command))
        return _result_from_run(
            run_id, backend, pattern, rate, run_dir, False, 0.0
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
            writer.writerow(
                {field: result.get(field, "") for field in FIELDNAMES}
            )


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
        value = min_value + step * index
        yield value


def _write_line_plot(
    rows: List[Dict[str, object]],
    pattern: str,
    y_key: str,
    y_label: str,
    title: str,
    path: Path,
) -> bool:
    series: Dict[str, List[tuple[float, float]]] = {}
    for row in rows:
        if row["pattern"] != pattern or not row.get("success"):
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
    left = 80
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

    colors = {
        "gem5": "#2f6f9f",
        "dramsys": "#b8423f",
    }
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
            f'font-family="sans-serif" font-size="11">{tick:.2g}</text>'
        )

    lines.append(
        f'<line x1="{left}" y1="{top + plot_height}" '
        f'x2="{left + plot_width}" y2="{top + plot_height}" '
        'stroke="#222"/>'
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
        f'<text x="18" y="{top + plot_height / 2}" '
        'text-anchor="middle" font-family="sans-serif" font-size="13" '
        f'transform="rotate(-90 18 {top + plot_height / 2})">'
        f"{html.escape(y_label)}</text>"
    )

    legend_y = 50
    for index, (backend, points) in enumerate(sorted(series.items())):
        color = colors.get(backend, "#444444")
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
        legend_x = left + 16 + index * 150
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
            f"{pattern}: achieved bandwidth",
            bandwidth_path,
        ):
            paths.append(bandwidth_path)

        latency_path = plot_dir / f"{pattern}-latency.svg"
        if _write_line_plot(
            results,
            pattern,
            "avg_latency_ticks",
            "Average latency (ticks)",
            f"{pattern}: average latency",
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
    if isinstance(value, float) and (
        math.isnan(value) or math.isinf(value)
    ):
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
        if row["pattern"] == pattern and row["backend"] == backend
        and row.get("success")
    ]
    if not rows:
        return None
    return max(rows, key=lambda row: float(row.get("achieved_GBps", 0.0)))


def _average(rows: List[Dict[str, object]], key: str) -> Optional[float]:
    values = []
    for row in rows:
        value = _clean_float(row.get(key))
        if value is not None:
            values.append(value)
    if not values:
        return None
    return sum(values) / len(values)


def _maximum(rows: List[Dict[str, object]], key: str) -> Optional[float]:
    values = []
    for row in rows:
        value = _clean_float(row.get(key))
        if value is not None:
            values.append(value)
    if not values:
        return None
    return max(values)


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
        "# CHI RN-I Memory Sweep Report",
        "",
        "## Methodology",
        "",
        "Synthetic TrafficGen requestors drive a cacheless CHI RN-I "
        "hierarchy. The path is TrafficGen -> CHI RN-I -> CHI HNF "
        "directory -> CHI SNF -> memory backend. Each run changes the "
        "offered bandwidth while holding the address range, cache line size, "
        "clock, and traffic pattern fixed.",
        "",
        "## Measurement Methodology",
        "",
        "Offered bandwidth is the programmed request injection rate, not the "
        "measured memory throughput. The sweep runner passes each listed rate "
        "to TrafficGen. TrafficGen converts that rate into a packet period of "
        "`block_size / rate`; with the default 64-byte block size, higher "
        "offered rates simply schedule 64-byte requests closer together. This "
        "report uses one TrafficGen requestor, so the programmed requestor "
        "rate and aggregate offered rate are the same. With multiple "
        "requestors, aggregate offered load is the sum of the programmed "
        "requestor rates.",
        "",
        "The linear patterns issue cache-line-sized requests in increasing "
        "address order and wrap at the end of the configured traffic range. "
        "The random patterns select a block-aligned address within the same "
        "range for each request. Read-only, write-only, and mixed patterns "
        "use TrafficGen's read percentage knob; the mixed runs in this report "
        "use a 50 percent read mix.",
        "",
        "Achieved bandwidth is measured from completed TrafficGen responses. "
        "The runner sums TrafficGen `readBW` and `writeBW`, where those stats "
        "are `bytesRead / simSeconds` and `bytesWritten / simSeconds`. The "
        "byte counters increment when a timing response returns, so achieved "
        "bandwidth reflects completed work rather than merely attempted "
        "injection. DRAMSys `AVG BW` and `MAX BW` are recorded separately as "
        "backend-local memory statistics.",
        "",
        "Average latency is also response based. TrafficGen records the tick "
        "when an accepted request is sent and adds the send-to-response time "
        "to the read or write latency total when the response arrives. At "
        "rates below saturation, increasing offered bandwidth usually raises "
        "queueing delay and achieved bandwidth. Past saturation, the memory "
        "path cannot accept requests at the requested pace. TrafficGen then "
        "stalls on timing-request backpressure and waiting-response limits; "
        "that excess pressure appears in retry counts, retry ticks, and Ruby "
        "buffer occupancy rather than as an unlimited number of already-sent "
        "requests. The accepted request rate is clipped near the sustainable "
        "throughput of the CHI/memory path, so the plotted average latency "
        "tends to bend upward and then flatten instead of growing without "
        "bound as offered bandwidth continues to increase.",
        "",
        "## Configuration",
        "",
        f"- gem5 binary: `{args.gem5_binary}`",
        f"- config script: `{args.config}`",
        f"- patterns: `{', '.join(patterns)}`",
        f"- backends: `{', '.join(backends)}`",
        f"- rates: `{', '.join(args.rates)}`",
        f"- duration: `{args.duration}`",
        f"- data limit: `{args.data_limit}`",
        f"- memory size: `{args.mem_size}`",
        f"- traffic address range: `{args.addr_range}`",
        f"- cache line size: `{args.cache_line_size}` bytes",
        f"- generator count: `{args.num_generators}`",
        f"- gem5 DDR4 interface: `{args.gem5_ddr4_interface}`",
        f"- DRAMSys config: `{args.dramsys_config}`",
        "",
        "## Caveats",
        "",
        "The default gem5 backend uses a local DDR4-1866 x8 4 GiB "
        "DRAMInterface derived from gem5's DDR4 timing interface and matched "
        "to the supplied DRAMSys gem5-SE DDR4 memspec. This keeps the "
        "comparison on one memory channel, the same 4 GiB exposed address "
        "range, the same cache line size, and comparable DDR4-1866 timing "
        "assumptions.",
        "",
        "The gem5 and DRAMSys timing models are not identical: gem5's "
        "DRAMInterface exposes a compact timing parameter set while DRAMSys "
        "uses its controller, address mapping, checker, and DRAMPower/"
        "DRAMUtils models. Treat the plots as backend-comparison data for "
        "this testcase rather than as device-validation measurements.",
        "",
        "Generated statistics are synthetic microbenchmark results and should "
        "be interpreted as configuration-comparison data, not as validated "
        "system-level memory measurements.",
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
        "## Raw Result Summary",
        "",
        "| Pattern | Backend | Peak achieved GB/s | Rate at peak | "
        "Latency at peak (ticks) | Host seconds |",
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
                f"{_format_number(best['avg_latency_ticks'], 1)} | "
                f"{_format_number(best['host_seconds'], 2)} |"
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

    if "gem5" in backends and "dramsys" in backends:
        gem5_rows = [
            row
            for row in results
            if row["backend"] == "gem5" and row.get("success")
        ]
        dramsys_rows = [
            row
            for row in results
            if row["backend"] == "dramsys" and row.get("success")
        ]
        gem5_wall = _average(gem5_rows, "wall_seconds")
        dramsys_wall = _average(dramsys_rows, "wall_seconds")
        if gem5_wall is not None and dramsys_wall is not None:
            wall_overhead = (dramsys_wall / gem5_wall - 1.0) * 100.0
            overhead_direction = "higher" if wall_overhead >= 0 else "lower"
            lines += [
                "",
                "For these short synthetic runs, DRAMSys average wall "
                f"runtime was {_format_number(abs(wall_overhead), 2)}% "
                f"{overhead_direction} than the gem5 MemCtrl backend.",
            ]

    lines += [
        "",
        "## Memory Stat Summary",
        "",
        "| Backend | Runs | Avg gem5 DRAM bus util | Max gem5 peak MiB/s | "
        "Avg DRAMSys AVG BW GB/s | Max DRAMSys MAX BW GB/s |",
        "|---|---:|---:|---:|---:|---:|",
    ]

    for backend in backends:
        rows = [
            row
            for row in results
            if row["backend"] == backend and row.get("success")
        ]
        lines.append(
            f"| {backend} | {len(rows)} | "
            f"{_format_number(_average(rows, 'gem5_dram_bus_util'))} | "
            f"{_format_number(_maximum(rows, 'gem5_dram_peak_MiBps'))} | "
            f"{_format_number(_average(rows, 'dramsys_avg_bw_GBps'))} | "
            f"{_format_number(_maximum(rows, 'dramsys_max_bw_GBps'))} |"
        )

    lines += [
        "",
        "## Queueing Summary",
        "",
        "| Pattern | Backend | Max RN/SNF request occupancy | "
        "Max retry ticks | Max avg latency (ticks) |",
        "|---|---|---:|---:|---:|",
    ]

    for pattern in patterns:
        for backend in backends:
            rows = [
                row
                for row in results
                if row["pattern"] == pattern and row["backend"] == backend
                and row.get("success")
            ]
            occupancy = _format_number(
                _maximum(rows, "request_buffer_occupancy")
            )
            lines.append(
                f"| {pattern} | {backend} | "
                f"{occupancy} | "
                f"{_format_number(_maximum(rows, 'retry_ticks'), 0)} | "
                f"{_format_number(_maximum(rows, 'avg_latency_ticks'), 1)} |"
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
            if row["pattern"] == pattern and row.get("success")
        ]
        if not rows:
            lines.append(f"- `{pattern}` did not produce successful runs.")
            continue
        best = max(rows, key=lambda row: float(row.get("achieved_GBps", 0.0)))
        lines.append(
            f"- `{pattern}` peaked at "
            f"{_format_number(best['achieved_GBps'])} GB/s with "
            f"`{best['backend']}` at offered rate `{best['rate']}`."
        )

    failed = [row for row in results if not row.get("success")]
    if failed:
        lines += [
            "",
            "## Failed Runs",
            "",
        ]
        for row in failed:
            lines.append(
                f"- `{row['run_id']}` failed; see `{row['run_dir']}`."
            )

    report.write_text("\n".join(lines) + "\n")


def _parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Sweep TrafficGen bandwidth through CHI RN-I memory paths."
    )
    parser.add_argument(
        "--gem5-binary",
        type=Path,
        default=Path("build/RISCV/gem5.opt"),
    )
    parser.add_argument("--config", type=Path, default=CONFIG)
    parser.add_argument(
        "--outdir",
        type=Path,
        default=Path("m5out/chi-rni-ddr4-memory-sweep"),
    )
    parser.add_argument(
        "--backends",
        default="gem5,dramsys",
        help="Comma-separated list: gem5, dramsys.",
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
        default="1866-x8-4gib",
        help="gem5 DDR4 interface name passed to the config script.",
    )
    parser.add_argument("--dram-addr-mapping", default=None)
    parser.add_argument(
        "--dramsys-config",
        default="ext/dramsys/gem5_configs/ddr4-gem5-se.json",
    )
    parser.add_argument("--dramsys-resource-dir", default=None)
    parser.add_argument("--sys-clock", default="3GHz")
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
                    saved_result = run_dir / "run_result.json"
                    wall_seconds = 0.0
                    if saved_result.exists():
                        wall_seconds = float(
                            json.loads(saved_result.read_text()).get(
                                "wall_seconds", 0.0
                            )
                        )
                    result = _result_from_run(
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
                            _json_safe(result),
                            indent=2,
                            allow_nan=False,
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
