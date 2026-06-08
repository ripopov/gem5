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

"""Run and report a DDR5-4800 gem5/Ramulator2 comparison."""

import argparse
import csv
import html
import json
import math
import re
import subprocess
import time
from pathlib import Path
from typing import Dict, Iterable, List, Optional

CONFIG = Path(
    "configs/example/gem5_library/trafficgen_memory/trafficgen-ddr5.py"
)

STANDARD_PATTERNS = [
    "linear-read",
    "linear-write",
    "linear-mixed",
    "random-read",
    "random-mixed",
]
CURVE_PATTERNS = ["probe-stream-read", "probe-stream-mixed"]
# Standard patterns that issue writes, so a write-completion latency view is
# meaningful. linear-read and random-read are pure reads and are excluded.
WRITE_BEARING_PATTERNS = ["linear-write", "linear-mixed", "random-mixed"]
BACKENDS = ["gem5", "ramulator"]
DEFAULT_STANDARD_RATES = [
    "1GiB/s",
    "4GiB/s",
    "8GiB/s",
    "16GiB/s",
    "24GiB/s",
    "32GiB/s",
    "40GiB/s",
    "48GiB/s",
]
DEFAULT_CURVE_RATES = [
    "1GiB/s",
    "4GiB/s",
    "8GiB/s",
    "12GiB/s",
    "16GiB/s",
    "20GiB/s",
    "24GiB/s",
    "28GiB/s",
    "32GiB/s",
    "36GiB/s",
    "40GiB/s",
    "48GiB/s",
]

BACKEND_COLORS = {"gem5": "#2f6f9f", "ramulator": "#b8423f"}
THEORETICAL_GBPS = 38.4
REFRESH_ADJUSTED_GBPS = THEORETICAL_GBPS * (
    1.0 - ((7.488 + 14.144 + 295.0 + 14.144) / 3900.0)
)

STAT_RE = re.compile(
    r"^([A-Za-z0-9_.$:/<>+-]+)\s+"
    r"([+-]?(?:nan|inf|\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?)"
)

FIELDNAMES = [
    "backend",
    "traffic_pattern",
    "traffic_mode",
    "read_percent",
    "rate",
    "offered_GBps",
    "achieved_GBps",
    "stream_GBps",
    "probe_GBps",
    "avg_latency_ns",
    "read_avg_latency_ns",
    "write_avg_latency_ns",
    "write_completion_latency_ns",
    "probe_avg_latency_ns",
    "completed_requests",
    "stream_completed_requests",
    "probe_completed_requests",
    "retry_ticks",
    "stream_retry_ticks",
    "probe_retry_ticks",
    "ramulator_total_GBps",
    "ramulator_avg_read_latency_ns",
    "ramulator_row_hit_rate",
    "sim_seconds",
    "host_seconds",
    "wall_seconds",
    "success",
    "exit_cause",
    "run_dir",
]


def _split_list(value: str) -> List[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def _sanitize(value: str) -> str:
    value = value.replace("/s", "ps").replace("/", "_")
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
        "KiB": 1024,
        "MiB": 1024**2,
        "GiB": 1024**3,
    }
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
            try:
                stats[match.group(1)] = float(match.group(2))
            except ValueError:
                pass
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


def _first_stat(stats: Dict[str, float], suffix: str) -> float:
    for key, value in stats.items():
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
    return sum(values) / len(values) if values else math.nan


def _load_metadata(path: Path) -> Dict[str, object]:
    if not path.exists():
        return {}
    try:
        return json.loads(path.read_text())
    except (ValueError, OSError):
        return {}


def _parse_ramulator_stats(path: Path) -> Dict[str, float]:
    parsed: Dict[str, float] = {}
    if not path.exists():
        return parsed

    totals = []
    latencies = []
    latency_weights = []
    row_hits = row_accesses = 0.0
    current: Dict[str, float] = {}

    def flush_controller() -> None:
        nonlocal row_hits, row_accesses
        if not current:
            return
        if "total_throughput_MBps" in current:
            totals.append(current["total_throughput_MBps"])
        reads = current.get("num_read_reqs_served", 0.0)
        if reads and "avg_read_latency" in current:
            latencies.append(current["avg_read_latency"] * reads)
            latency_weights.append(reads)
        hits = current.get("row_hits", 0.0)
        misses = current.get("row_misses", 0.0)
        conflicts = current.get("row_conflicts", 0.0)
        row_hits += hits
        row_accesses += hits + misses + conflicts

    for raw in path.read_text(errors="replace").splitlines():
        if raw.strip() == "controller:":
            flush_controller()
            current = {}
            continue
        match = re.match(r"\s{4}([A-Za-z0-9_]+):\s+([-+0-9.eE]+)$", raw)
        if not match:
            continue
        try:
            current[match.group(1)] = float(match.group(2))
        except ValueError:
            pass
    flush_controller()

    if totals:
        parsed["ramulator_total_GBps"] = sum(totals) / 1000.0
    if latency_weights:
        parsed["ramulator_avg_read_latency_ns"] = (
            sum(latencies) / sum(latency_weights) * 0.416
        )
    if row_accesses:
        parsed["ramulator_row_hit_rate"] = row_hits / row_accesses
    return parsed


def _latency_ns(
    total_ticks: float,
    count: float,
    tick_to_ns: float,
) -> float:
    return total_ticks / count * tick_to_ns if count else math.nan


def _scope_metrics(
    stats: Dict[str, float],
    required: str,
    tick_to_ns: float,
) -> Dict[str, float]:
    reads = _sum_stats(stats, ".totalReads", required)
    writes = _sum_stats(stats, ".totalWrites", required)
    read_lat = _sum_stats(stats, ".totalReadLatency", required)
    write_lat = _sum_stats(stats, ".totalWriteLatency", required)
    read_bw = _sum_stats(stats, ".readBW", required)
    write_bw = _sum_stats(stats, ".writeBW", required)
    total = reads + writes
    return {
        "reads": reads,
        "writes": writes,
        "requests": total,
        "read_GBps": read_bw / 1e9,
        "write_GBps": write_bw / 1e9,
        "GBps": (read_bw + write_bw) / 1e9,
        "retry_ticks": _sum_stats(stats, ".retryTicks", required),
        "read_latency_ns": _latency_ns(read_lat, reads, tick_to_ns),
        "write_latency_ns": _latency_ns(write_lat, writes, tick_to_ns),
        "avg_latency_ns": _latency_ns(read_lat + write_lat, total, tick_to_ns),
    }


def _write_completion_latency_ns(
    stats: Dict[str, float],
    tick_to_ns: float,
) -> float:
    """True (enqueue-to-DRAM-commit) write latency from backend stats.

    Both backends now post writes (the requestor is acknowledged at
    write-buffer enqueue), so neither backend's requestor-visible write
    latency reflects the real DRAM write. Each backend instead exposes an
    internal enqueue-to-commit counter:

    - gem5 ``MemCtrl`` accumulates ``readyTime - entryTime`` into
      ``requestorWriteTotalLat`` per requestor (see
      ``MemCtrl::doBurstAccess``), with ``requestorWriteAccesses`` as the
      count. Summed across both channel controllers.
    - The Ramulator2 wrapper accumulates the enqueue-to-completion-callback
      latency into ``totalWriteCompletionLatency`` with ``writeCompletions``
      as the count.

    Returns the mean in ns, or NaN if the run issued no writes.
    """
    def _sum_substr(token: str) -> float:
        total = 0.0
        for key, value in stats.items():
            if token in key and not math.isnan(value):
                total += value
        return total

    # gem5 MemCtrl per-requestor latency vectors (suffix has a ::requestor).
    gem5_lat = _sum_substr(".requestorWriteTotalLat::")
    gem5_cnt = _sum_substr(".requestorWriteAccesses::")
    if gem5_cnt > 0:
        return gem5_lat / gem5_cnt * tick_to_ns

    # Ramulator2 wrapper scalars.
    ram_lat = _sum_substr(".totalWriteCompletionLatency")
    ram_cnt = _sum_substr(".writeCompletions")
    if ram_cnt > 0:
        return ram_lat / ram_cnt * tick_to_ns

    return math.nan


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
    sim_ticks = _first_stat(stats, "simTicks")
    sim_seconds = _first_stat(stats, "simSeconds")
    tick_to_ns = (
        (sim_seconds / sim_ticks) * 1e9
        if sim_ticks and sim_seconds and not math.isnan(sim_ticks)
        else 1.0e-3
    )

    traffic_mode = str(metadata.get("traffic_mode", "standard"))
    if traffic_mode == "probe-stream":
        num_streams = int(metadata.get("num_stream_generators", 1))
        stream_parts = [
            _scope_metrics(
                stats, f"processor.cores{index}.generator", tick_to_ns
            )
            for index in range(num_streams)
        ]
        stream = {
            "reads": sum(part["reads"] for part in stream_parts),
            "writes": sum(part["writes"] for part in stream_parts),
            "requests": sum(part["requests"] for part in stream_parts),
            "read_GBps": sum(part["read_GBps"] for part in stream_parts),
            "write_GBps": sum(part["write_GBps"] for part in stream_parts),
            "GBps": sum(part["GBps"] for part in stream_parts),
            "retry_ticks": sum(part["retry_ticks"] for part in stream_parts),
            "read_latency_ns": math.nan,
            "write_latency_ns": math.nan,
            "avg_latency_ns": math.nan,
        }
        probe = _scope_metrics(
            stats, f"processor.cores{num_streams}.generator", tick_to_ns
        )
        primary_latency = probe["read_latency_ns"]
    else:
        stream = _scope_metrics(stats, "processor", tick_to_ns)
        probe = {
            "GBps": 0.0,
            "requests": 0.0,
            "retry_ticks": 0.0,
            "read_latency_ns": math.nan,
        }
        primary_latency = stream["avg_latency_ns"]

    ramulator = _parse_ramulator_stats(run_dir / "ramulator_stats.yaml")
    achieved = stream["GBps"] + probe["GBps"]

    # Write-completion latency: a backend-honest write latency measured at
    # the point the write is actually committed to DRAM, not when the
    # requestor receives its response. Both backends post writes (ack at
    # write-buffer enqueue), so the requestor-visible write latency is the
    # posted ack on both, not real write timing. This value instead comes
    # from each backend's internal enqueue-to-commit counter.
    write_completion_latency = _write_completion_latency_ns(
        stats, tick_to_ns
    )
    offered_multiplier = (
        int(metadata.get("num_stream_generators", 1))
        if traffic_mode == "probe-stream"
        else 1
    )

    return {
        "backend": backend,
        "traffic_pattern": pattern,
        "traffic_mode": traffic_mode,
        "read_percent": metadata.get("read_percent", ""),
        "rate": rate,
        "offered_GBps": _parse_rate_to_Bps(rate) / 1e9 * offered_multiplier,
        "achieved_GBps": achieved,
        "stream_GBps": stream["GBps"],
        "probe_GBps": probe["GBps"],
        "avg_latency_ns": primary_latency,
        "read_avg_latency_ns": stream["read_latency_ns"],
        "write_avg_latency_ns": stream["write_latency_ns"],
        "write_completion_latency_ns": write_completion_latency,
        "probe_avg_latency_ns": probe["read_latency_ns"],
        "completed_requests": stream["requests"] + probe["requests"],
        "stream_completed_requests": stream["requests"],
        "probe_completed_requests": probe["requests"],
        "retry_ticks": stream["retry_ticks"] + probe["retry_ticks"],
        "stream_retry_ticks": stream["retry_ticks"],
        "probe_retry_ticks": probe["retry_ticks"],
        "ramulator_total_GBps": ramulator.get(
            "ramulator_total_GBps", math.nan
        ),
        "ramulator_avg_read_latency_ns": ramulator.get(
            "ramulator_avg_read_latency_ns", math.nan
        ),
        "ramulator_row_hit_rate": ramulator.get(
            "ramulator_row_hit_rate", math.nan
        ),
        "sim_seconds": sim_seconds,
        "host_seconds": _first_stat(stats, "hostSeconds"),
        "wall_seconds": wall_seconds,
        "success": success,
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

    num_generators = (
        args.curve_stream_generators
        if pattern.startswith("probe-stream")
        else args.num_generators
    )
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
        "--probe-rate",
        args.probe_rate,
        "--duration",
        args.duration,
        "--data-limit",
        args.data_limit,
        "--probe-data-limit",
        args.probe_data_limit,
        "--addr-range",
        args.addr_range,
        "--probe-addr-range",
        args.probe_addr_range,
        "--mem-size",
        args.mem_size,
        "--num-generators",
        str(num_generators),
        "--cache-line-size",
        str(args.cache_line_size),
        "--mixed-read-percent",
        str(args.mixed_read_percent),
        "--dram-addr-mapping",
        args.dram_addr_mapping,
        "--sys-clock",
        args.sys_clock,
        "--ramulator-python-path",
        args.ramulator_python_path,
        "--curve-stream-generator",
        args.curve_stream_generator,
        "--stream-num-seq-pkts",
        str(args.stream_num_seq_pkts),
        "--stream-page-size",
        args.stream_page_size,
        "--stream-banks",
        str(args.stream_banks),
        "--stream-banks-util",
        str(args.stream_banks_util),
    ]
    (run_dir / "command.json").write_text(json.dumps(command, indent=2) + "\n")

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
        writer = csv.DictWriter(
            csv_file, fieldnames=FIELDNAMES, lineterminator="\n"
        )
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


def _json_safe(value):
    if isinstance(value, dict):
        return {key: _json_safe(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_json_safe(item) for item in value]
    if isinstance(value, float) and (math.isnan(value) or math.isinf(value)):
        return None
    return value


def _axis_ticks(min_value: float, max_value: float, count: int = 5):
    if max_value <= min_value:
        max_value = min_value + 1.0
    step = (max_value - min_value) / (count - 1)
    for index in range(count):
        yield min_value + step * index


def _write_line_plot(
    rows: List[Dict[str, object]],
    pattern: str,
    x_key: str,
    y_key: str,
    x_label: str,
    y_label: str,
    title: str,
    path: Path,
) -> bool:
    series: Dict[str, List[tuple]] = {}
    for row in rows:
        if row["traffic_pattern"] != pattern or not row.get("success"):
            continue
        x_value = _clean_float(row.get(x_key))
        y_value = _clean_float(row.get(y_key))
        if x_value is None or y_value is None:
            continue
        series.setdefault(str(row["backend"]), []).append((x_value, y_value))
    for values in series.values():
        values.sort()
    points = [point for values in series.values() for point in values]
    if not points:
        return False

    width, height = 760, 460
    left, right, top, bottom = 88, 24, 48, 68
    plot_width = width - left - right
    plot_height = height - top - bottom
    x_min = 0.0
    x_max = max(point[0] for point in points) * 1.04
    y_min = 0.0
    y_max = max(point[1] for point in points) * 1.08 or 1.0

    def sx(value: float) -> float:
        return left + (value - x_min) * plot_width / (x_max - x_min or 1.0)

    def sy(value: float) -> float:
        return (
            top
            + plot_height
            - (value - y_min) * plot_height / (y_max - y_min or 1.0)
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
        f"{html.escape(x_label)}</text>"
    )
    lines.append(
        f'<text x="20" y="{top + plot_height / 2}" '
        'text-anchor="middle" font-family="sans-serif" font-size="13" '
        f'transform="rotate(-90 20 {top + plot_height / 2})">'
        f"{html.escape(y_label)}</text>"
    )
    if "bandwidth" in x_label.lower():
        for value, label, color, anchor, x_offset, y_offset in [
            (
                REFRESH_ADJUSTED_GBPS,
                "refresh-adjusted",
                "#4f8f3a",
                "end",
                -4,
                14,
            ),
            (
                THEORETICAL_GBPS,
                "theoretical",
                "#d98500",
                "start",
                4,
                30,
            ),
        ]:
            if value <= x_max:
                x = sx(value)
                lines.append(
                    f'<line x1="{x:.1f}" y1="{top}" x2="{x:.1f}" '
                    f'y2="{top + plot_height}" stroke="{color}" '
                    'stroke-dasharray="5 4"/>'
                )
                lines.append(
                    f'<text x="{x + x_offset:.1f}" '
                    f'y="{top + y_offset}" text-anchor="{anchor}" '
                    f'font-family="sans-serif" font-size="11" '
                    f'fill="{color}">{label}</text>'
                )

    legend_y = 50
    for index, (backend, values) in enumerate(sorted(series.items())):
        color = BACKEND_COLORS.get(backend, "#444444")
        polyline = " ".join(f"{sx(x):.1f},{sy(y):.1f}" for x, y in values)
        lines.append(
            f'<polyline fill="none" stroke="{color}" stroke-width="2.2" '
            f'points="{polyline}"/>'
        )
        for x, y in values:
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
    results: List[Dict[str, object]], plot_dir: Path
) -> List[Path]:
    plot_dir.mkdir(parents=True, exist_ok=True)
    paths = []
    for pattern in STANDARD_PATTERNS:
        for y_key, y_label, stem in [
            ("achieved_GBps", "Achieved bandwidth (GB/s)", "bandwidth"),
            ("avg_latency_ns", "Average latency (ns)", "latency"),
        ]:
            path = plot_dir / f"{pattern}-{stem}.svg"
            if _write_line_plot(
                results,
                pattern,
                "offered_GBps",
                y_key,
                "Offered bandwidth (GB/s)",
                y_label,
                f"{pattern}: {y_label}",
                path,
            ):
                paths.append(path)
        if pattern in WRITE_BEARING_PATTERNS:
            path = plot_dir / f"{pattern}-write-completion-latency.svg"
            if _write_line_plot(
                results,
                pattern,
                "offered_GBps",
                "write_completion_latency_ns",
                "Offered bandwidth (GB/s)",
                "Write-completion latency (ns)",
                f"{pattern}: write-completion latency",
                path,
            ):
                paths.append(path)
    for pattern in CURVE_PATTERNS:
        path = plot_dir / f"{pattern}-latency-bandwidth.svg"
        if _write_line_plot(
            results,
            pattern,
            "stream_GBps",
            "probe_avg_latency_ns",
            "Used stream bandwidth (GB/s)",
            "Random-read probe latency (ns)",
            f"{pattern}: bandwidth-latency curve",
            path,
        ):
            paths.append(path)
    return paths


def _format_number(value: object, digits: int = 3) -> str:
    number = _clean_float(value)
    if number is None:
        return "n/a"
    return f"{number:.{digits}f}"


def _rows(
    results: List[Dict[str, object]],
    pattern: Optional[str] = None,
    backend: Optional[str] = None,
):
    rows = [row for row in results if row.get("success")]
    if pattern is not None:
        rows = [row for row in rows if row["traffic_pattern"] == pattern]
    if backend is not None:
        rows = [row for row in rows if row["backend"] == backend]
    return rows


def _best(
    results: List[Dict[str, object]],
    pattern: str,
    backend: str,
    key: str = "achieved_GBps",
) -> Optional[Dict[str, object]]:
    rows = _rows(results, pattern, backend)
    if not rows:
        return None
    return max(rows, key=lambda row: _clean_float(row.get(key)) or 0.0)


def _low_load(
    results: List[Dict[str, object]],
    pattern: str,
    backend: str,
) -> Optional[Dict[str, object]]:
    rows = _rows(results, pattern, backend)
    rows = [row for row in rows if str(row["rate"]) == "1GiB/s"]
    return rows[0] if rows else None


def _peak_offered(
    results: List[Dict[str, object]],
    pattern: str,
    backend: str,
) -> Optional[Dict[str, object]]:
    rows = _rows(results, pattern, backend)
    if not rows:
        return None
    return max(rows, key=lambda row: _clean_float(row.get("offered_GBps")) or 0.0)


def _write_report(
    args: argparse.Namespace,
    results: List[Dict[str, object]],
    plots: List[Path],
) -> None:
    lines = [
        "# DDR5-4800 Native gem5 vs Ramulator2",
        "",
        "This report compares native gem5 `MemCtrl`/`DRAMInterface` against "
        "Ramulator2 v2.1 through the same cacheless TrafficGen path.",
        "",
        "## Device Under Test",
        "",
        "| Parameter | Value |",
        "| --- | --- |",
        "| Standard | DDR5-4800AN-style |",
        "| Channels | 2 independent 32-bit channels |",
        "| Ranks | 1 rank per channel |",
        "| Devices | 4 x8 devices per rank |",
        "| Device density | 16Gb |",
        "| Total capacity | 16GiB |",
        "| Burst | BL16, 64-byte transaction per channel |",
        "| Banks | 8 bank groups x 4 banks = 32 banks/rank |",
        "| tCK | 0.416ns |",
        "| Peak bandwidth | 38.4 GB/s |",
        f"| Refresh-adjusted sanity line | {REFRESH_ADJUSTED_GBPS:.2f} GB/s |",
        "",
        "The Ramulator2 backend uses `DDR5_16Gb_x8` and `DDR5_4800AN`. "
        "The native gem5 backend uses a matching custom "
        "`DDR5_4800_4x8_16GiB` interface in the testbench.",
        "",
        "## Methodology",
        "",
        "This section describes what every measurement in the report means. "
        "The intent is to compare two memory backends under the same "
        "requestor-visible workload, while avoiding the common mistake of "
        "measuring an idle latency or a core/cache statistic and treating it "
        "as loaded DRAM latency.",
        "",
        "### Test Topology",
        "",
        "Every run uses the same cacheless path:",
        "",
        "```text",
        "TrafficGen -> SystemXBar/NoCache -> memory backend",
        "```",
        "",
        "The memory backend is either native gem5 `MemCtrl` plus a custom "
        "`DDR5_4800_4x8_16GiB` `DRAMInterface`, or the new Ramulator2 "
        "SimObject configured with Ramulator2's `DDR5_16Gb_x8` organization "
        "and `DDR5_4800AN` timing preset. Both backends expose the same "
        "16GiB address range and the same two 32-bit DDR5 channel topology.",
        "",
        "### Units and Primary Metrics",
        "",
        "- Offered rates are specified in binary `GiB/s` on the command line "
        "because gem5's rate parser uses binary suffixes.",
        "- Reported bandwidth is decimal `GB/s` from completed TrafficGen "
        "responses. This is the used bandwidth observed by the requestor, not "
        "the requested injection rate.",
        "- Reported latency is requestor-visible send-to-response latency for "
        "completed requests, converted from gem5 ticks to ns.",
        "- Ramulator2 backend-local counters are recorded separately and used "
        "only as sanity checks. The tables compare the same TrafficGen "
        "response counters for both backends.",
        "",
        "### Reference Bandwidth Lines",
        "",
        "The SVG plots include two vertical reference lines when the x-axis is "
        "a bandwidth axis.",
        "",
        "- `theoretical` is the raw bus-rate limit: two 32-bit channels at "
        "4800 MT/s, or `2 x 32 bits x 4800e6 / 8 = 38.4 GB/s`. It assumes "
        "every transfer slot carries useful data and ignores refresh, "
        "turnaround, command scheduling, row misses, and queue effects.",
        "- `refresh-adjusted` is a DDR5 sanity upper bound that removes the "
        "time lost to periodic all-bank refresh. It uses "
        "`38.4 GB/s x (1 - (tRTP + tRP + tRFC + tRCD) / tREFI)`, with "
        "`tRTP=7.488ns`, `tRP=14.144ns`, `tRFC=295ns`, `tRCD=14.144ns`, "
        f"and `tREFI=3900ns`, giving {REFRESH_ADJUSTED_GBPS:.2f} GB/s.",
        "- These lines are sanity references, not pass/fail thresholds. In "
        "offered-load plots they mark where the requested traffic reaches the "
        "nominal device limit. In bandwidth-latency curve plots they mark "
        "the used stream bandwidth that would correspond to those limits.",
        "",
        "### Standard Sweeps",
        "",
        "The standard plots use one TrafficGen requestor to sweep offered "
        "load over a fixed address range and record completed bandwidth and "
        "average request latency.",
        "",
        "- `linear-read`: sequential cache-line reads. This favors row-buffer "
        "locality and should approach the best read bandwidth.",
        "- `linear-write`: sequential cache-line writes. This stresses write "
        "timing, write queueing, and write-drain policy.",
        "- `linear-mixed`: sequential traffic with a 50/50 read/write mix.",
        "- `random-read`: random cache-line reads over the address range. "
        "This intentionally destroys most row locality.",
        "- `random-mixed`: random traffic with a 50/50 read/write mix.",
        "",
        "For these plots, the x-axis is offered bandwidth and the y-axis is "
        "either completed bandwidth or average latency. A flat completed "
        "bandwidth curve at high offered load indicates saturation or "
        "back-pressure. Rising latency indicates queueing inside the memory "
        "system or interconnect path.",
        "",
        "### Probe-Stream Curves",
        "",
        "`probe-stream-read` and `probe-stream-mixed` are the hockey-stick "
        "latency-bandwidth measurements. They use separate requestors so "
        "the stream traffic creates load while an independent random-read "
        "probe measures loaded latency.",
        "",
        "- The stream requestor uses gem5's DRAM-aware TrafficGen primitive. "
        "It issues short row-hit bursts and rotates across all 32 banks, "
        "which is closer to the paper's requirement that stream requests "
        "exploit bank-group and bank-level parallelism.",
        "- The probe requestor always issues random reads with "
        "`max_outstanding_reqs = 1`. This keeps the probe latency from being "
        "hidden by multiple in-flight probe requests.",
        "- `probe-stream-read` uses a 100% read stream plus the random-read "
        "probe.",
        "- `probe-stream-mixed` uses a 50/50 read/write stream plus the same "
        "random-read probe.",
        "- The curve x-axis is completed stream bandwidth only. The probe's "
        "small bandwidth is excluded from the x-axis so the curve shows how "
        "background stream load affects random-read latency.",
        "- The curve y-axis is the probe requestor's read latency. This is "
        "the loaded latency measurement; it should rise when the memory "
        "system queues requests near saturation.",
        "",
        "These curves are the closest match to the methodology criticized in "
        "`docs/ramulator-paper/source`: load the memory with stream traffic, "
        "measure random-read probe latency, and sanity-check against the "
        "theoretical and refresh-adjusted bandwidth limits.",
        "",
        "### Write Latency: Posted Writes on Both Backends",
        "",
        "Both backends now treat writes as **posted**: the requestor is "
        "acknowledged as soon as the write is accepted into the write "
        "buffer, and the write drains to DRAM asynchronously. This makes "
        "requestor-visible write back-pressure symmetric -- the requestor "
        "stalls only when the write buffer is full -- so the same metric "
        "means the same thing on both sides.",
        "",
        "- Native gem5 `MemCtrl` posts writes by design: `addToWriteQueue()` "
        "responds to the requestor the instant the write is accepted into "
        "the write buffer, adding only the static `frontendLatency` "
        "(`accessAndRespond(pkt, frontendLatency, ...)` in "
        "`src/mem/mem_ctrl.cc`).",
        "- The Ramulator2 backend originally responded only on DRAM "
        "completion, which throttled the requestor by completion latency "
        "rather than write-buffer occupancy. It now posts writes too (the "
        "`post_writes` parameter, default on): on a successful enqueue it "
        "acknowledges the requestor after `write_frontend_latency` (matched "
        "to gem5's `static_frontend_latency`) and uses Ramulator2's own "
        "buffer-full signal for back-pressure, exactly like gem5. The write "
        "still drains to DRAM asynchronously inside Ramulator2.",
        "- gem5 reads, and Ramulator2 reads, both respond at DRAM completion "
        "(`readyTime` plus `frontendLatency + backendLatency` for gem5), so "
        "read bandwidth and latency are directly comparable. With posted "
        "writes the requestor-visible write latency is the posted ack on "
        "both backends -- a low, near-constant number that is **not** a real "
        "write latency. Use the write-completion latency below for real "
        "write timing.",
        "",
        "### Write-Completion Latency (controller-honest)",
        "",
        "Because both backends post writes, neither one's requestor-visible "
        "write latency reflects the real DRAM write. The report therefore "
        "reads a write-completion latency from each backend's internal "
        "enqueue-to-commit counter, independent of when the requestor was "
        "acknowledged.",
        "",
        "- For gem5 this is `requestorWriteTotalLat / requestorWriteAccesses`, "
        "summed across both channel controllers. `MemCtrl::doBurstAccess` "
        "accumulates `readyTime - entryTime` (enqueue to DRAM-ready) into "
        "`requestorWriteTotalLat`.",
        "- For Ramulator2 the wrapper records the enqueue tick of each write "
        "and, in the DRAM-completion callback, accumulates "
        "`curTick() - enqueueTick` into `totalWriteCompletionLatency` with "
        "`writeCompletions` as the count -- the direct analogue of gem5's "
        "counter.",
        "- This view exposes real controller behavior the posted-write metric "
        "hides. gem5's write-drain policy batches writes, so at low offered "
        "load a write can sit in the buffer until the drain threshold is "
        "reached (high enqueue-to-commit latency), dropping as load rises. "
        "Ramulator2 drains promptly at low load and climbs toward its write "
        "service limit under load.",
        "",
        "### Known Interpretation Limits",
        "",
        "The native gem5 and Ramulator2 models are not identical controller "
        "implementations. With posted writes the requestor-visible write "
        "back-pressure mechanism now matches (buffer-full stall on both), so "
        "the residual write-bandwidth gap reflects a genuine difference in "
        "sustained DRAM write-drain rate, not a measurement artifact: gem5's "
        "`MemCtrl` write-drain policy and Ramulator2's `GenericDDR` scheduler "
        "batch and pipeline writes differently. Read-heavy bandwidth and "
        "probe-stream peak bandwidth remain the most directly comparable "
        "measurements.",
        "",
        "## External Context",
        "",
        "- The local `docs/ramulator-paper` source and the public "
        "[DDR5-4800AN Ramulator2 latency-bandwidth figure]"
        "(https://www.researchgate.net/figure/"
        "Latency-bandwidth-curves-for-DDR5-4800AN-in-Ramulator-20-using-the-"
        "Mess-Request_fig2_396692751) describe the same measurement "
        "discipline: generate streaming load, measure only random read probe "
        "latency, and sanity check against theoretical and refresh-adjusted "
        "DDR5 bandwidth.",
        "- The [Mess benchmark project]"
        "(https://memory.bsc.es/tools/mess-benchmark) describes curve "
        "construction by varying traffic intensity and read/write ratio; the "
        "[MICRO 2024 paper](https://arxiv.org/abs/2405.10170) uses "
        "bandwidth-latency curves across DDR4, DDR5, HBM, and CXL systems.",
        "- Public DDR5 references are not directly comparable to this "
        "cacheless two-subchannel simulator setup, but they are useful scale "
        "checks: [PassMark's live DDR5 latency chart]"
        "(https://www.memorybenchmark.net/latency_ddr5.html) shows DDR5 "
        "system latencies in the tens of ns, while DDR5-4800 bandwidth "
        "[tables](https://www.computerbase.de/artikel/arbeitsspeicher/"
        "ddr5-arbeitsspeicher-bandbreiten-latenzen.80697/) report 38.4 GB/s "
        "for a 64-bit DIMM-equivalent interface. This testbench models the "
        "same aggregate width as two 32-bit subchannels.",
        "",
        "## Plots",
        "",
    ]
    for plot in plots:
        lines.append(f"![{plot.stem}]({plot.relative_to(args.outdir)})")
        lines.append("")

    lines += [
        "## Peak Achieved Bandwidth",
        "",
        "`Latency ns` here is the requestor-visible send-to-response latency. "
        "For write-bearing patterns on gem5 this reflects the posted "
        "(early) write acknowledgement, not real write latency -- see the "
        "Write Completion Latency table.",
        "",
        "| Pattern | Backend | Peak GB/s | Rate | Latency ns | Host s |",
        "|---|---|---:|---|---:|---:|",
    ]
    for pattern in STANDARD_PATTERNS:
        for backend in BACKENDS:
            row = _best(results, pattern, backend)
            if not row:
                continue
            lines.append(
                f"| {pattern} | {backend} | "
                f"{_format_number(row['achieved_GBps'])} | "
                f"{row['rate']} | {_format_number(row['avg_latency_ns'], 1)} "
                f"| {_format_number(row['host_seconds'], 3)} |"
            )

    lines += [
        "",
        "## Low-Load Latency",
        "",
        "`Write resp ns` is the requestor-visible write latency. Both "
        "backends post writes, so this is the posted write-buffer "
        "acknowledgement on both -- a low number, not a real write latency. "
        "`Write commit ns` is the backend-honest write-completion latency "
        "(enqueue to DRAM commit) and is the column to compare across "
        "backends.",
        "",
        "| Pattern | Backend | Avg ns | Read ns | Write resp ns | "
        "Write commit ns |",
        "|---|---|---:|---:|---:|---:|",
    ]
    for pattern in STANDARD_PATTERNS:
        for backend in BACKENDS:
            row = _low_load(results, pattern, backend)
            if not row:
                continue
            lines.append(
                f"| {pattern} | {backend} | "
                f"{_format_number(row['avg_latency_ns'], 1)} | "
                f"{_format_number(row['read_avg_latency_ns'], 1)} | "
                f"{_format_number(row['write_avg_latency_ns'], 1)} | "
                f"{_format_number(row['write_completion_latency_ns'], 1)} |"
            )

    lines += [
        "",
        "## Write Completion Latency",
        "",
        "Backend-honest write latency measured at DRAM commit from each "
        "backend's internal enqueue-to-commit counter (gem5: "
        "`requestorWriteAvgLat`; Ramulator2: `avgWriteCompletionLatency`). "
        "`Posted resp ns` is the requestor-visible posted acknowledgement "
        "shown for contrast -- it is the early write-buffer ack on both "
        "backends, not a real write latency.",
        "",
        "| Pattern | Backend | Low-load commit ns | Peak-load commit ns | "
        "Posted resp ns |",
        "|---|---|---:|---:|---:|",
    ]
    for pattern in WRITE_BEARING_PATTERNS:
        for backend in BACKENDS:
            low = _low_load(results, pattern, backend)
            peak = _peak_offered(results, pattern, backend)
            if not peak:
                continue
            lines.append(
                f"| {pattern} | {backend} | "
                f"{_format_number(low['write_completion_latency_ns'], 1) if low else 'n/a'} | "
                f"{_format_number(peak['write_completion_latency_ns'], 1)} | "
                f"{_format_number(peak['write_avg_latency_ns'], 1)} |"
            )

    lines += [
        "",
        "## Hockey-Stick Curve Summary",
        "",
        "| Pattern | Backend | Max stream GB/s | Probe latency at max ns | "
        "Low-load probe ns |",
        "|---|---|---:|---:|---:|",
    ]
    for pattern in CURVE_PATTERNS:
        for backend in BACKENDS:
            row = _best(results, pattern, backend, "stream_GBps")
            low = _low_load(results, pattern, backend)
            if not row:
                continue
            lines.append(
                f"| {pattern} | {backend} | "
                f"{_format_number(row['stream_GBps'])} | "
                f"{_format_number(row['probe_avg_latency_ns'], 1)} | "
                f"{_format_number(low['probe_avg_latency_ns'], 1) if low else 'n/a'} |"
            )

    lines += [
        "",
        "## Backend-Local Ramulator2 Sanity Counters",
        "",
        "| Pattern | Peak Ramulator2 GB/s | Avg read latency ns | Row-hit rate |",
        "|---|---:|---:|---:|",
    ]
    for pattern in STANDARD_PATTERNS + CURVE_PATTERNS:
        row = _best(results, pattern, "ramulator")
        if not row:
            continue
        lines.append(
            f"| {pattern} | {_format_number(row['ramulator_total_GBps'])} | "
            f"{_format_number(row['ramulator_avg_read_latency_ns'], 1)} | "
            f"{_format_number(row['ramulator_row_hit_rate'])} |"
        )

    lines += [
        "",
        "## Interpretation",
        "",
    ]
    for pattern in STANDARD_PATTERNS:
        gem5 = _best(results, pattern, "gem5")
        ram = _best(results, pattern, "ramulator")
        if not gem5 or not ram:
            continue
        g_bw = _clean_float(gem5["achieved_GBps"]) or 0.0
        r_bw = _clean_float(ram["achieved_GBps"]) or 0.0
        if min(g_bw, r_bw) <= 0:
            continue
        delta = abs(g_bw - r_bw) / max(g_bw, r_bw)
        if delta > 0.2:
            lines.append(
                f"- `{pattern}` peak bandwidth differs substantially "
                f"({g_bw:.2f} GB/s gem5 vs {r_bw:.2f} GB/s Ramulator2). "
                "Both backends post writes, so the requestor-visible write "
                "back-pressure mechanism is now the same (stall only when the "
                "write buffer is full). The remaining gap is a genuine "
                "difference in sustained DRAM write-drain rate between gem5's "
                "`MemCtrl` write-drain policy and Ramulator2's `GenericDDR` "
                "scheduler, not a measurement artifact. See the Write "
                "Completion Latency table for the backend-honest write timing."
            )
    for pattern in CURVE_PATTERNS:
        gem5 = _best(results, pattern, "gem5", "stream_GBps")
        ram = _best(results, pattern, "ramulator", "stream_GBps")
        if not gem5 or not ram:
            continue
        g_bw = _clean_float(gem5["stream_GBps"]) or 0.0
        r_bw = _clean_float(ram["stream_GBps"]) or 0.0
        if min(g_bw, r_bw) > 0:
            delta = abs(g_bw - r_bw) / max(g_bw, r_bw)
            if delta > 0.2:
                lines.append(
                    f"- `{pattern}` has a large peak-bandwidth mismatch "
                    f"({g_bw:.2f} GB/s gem5 vs {r_bw:.2f} GB/s "
                    "Ramulator2). Treat this as a model difference until "
                    "command traces are compared; likely causes are native "
                    "gem5's controller pipeline/write-drain policy and "
                    "Ramulator2's DDR5 timing/scheduler details."
                )
            else:
                lines.append(
                    f"- `{pattern}` peak stream bandwidth agrees within "
                    f"{delta * 100:.1f}% between backends."
                )
    lines += [
        "- Native gem5 includes explicit fixed frontend/backend controller "
        "latencies. Ramulator2 reports lower backend-local read latency in "
        "cycles, but the report's primary read latency is the same TrafficGen "
        "requestor-visible send-to-response metric for both backends. Writes "
        "are posted on both backends, so their real timing comes from each "
        "backend's internal enqueue-to-commit counter (see Write Completion "
        "Latency), not the requestor-visible posted ack.",
        "- The refresh-adjusted line is a sanity bound, not a pass/fail "
        "criterion. TrafficGen request timing, queue back-pressure, write "
        "turnaround, and row locality can keep achieved bandwidth below it.",
        "",
        "## Reproduction",
        "",
        "```sh",
        "git clone --branch v2.1 --depth 1 "
        "https://github.com/CMU-SAFARI/ramulator2.git "
        "ext/ramulator2/ramulator2",
        "cmake -S ext/ramulator2/ramulator2 "
        "-B ext/ramulator2/ramulator2/build "
        "-DRAMULATOR_PYTHON_BINDINGS=OFF",
        "cmake --build ext/ramulator2/ramulator2/build -j",
        "scons build/RISCV/gem5.opt -j6",
        "python3 util/trafficgen_memory_sweep/run_trafficgen_ddr5_sweep.py",
        "```",
    ]

    (args.outdir / "report.md").write_text("\n".join(lines) + "\n")


def _parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--gem5-binary", type=Path, default=Path("build/RISCV/gem5.opt")
    )
    parser.add_argument("--config", type=Path, default=CONFIG)
    parser.add_argument(
        "--outdir",
        type=Path,
        default=Path("m5out/ddr5-4800-gem5-ramulator-sweep"),
    )
    parser.add_argument("--backends", default=",".join(BACKENDS))
    parser.add_argument(
        "--standard-patterns", default=",".join(STANDARD_PATTERNS)
    )
    parser.add_argument("--curve-patterns", default=",".join(CURVE_PATTERNS))
    parser.add_argument(
        "--standard-rates", default=",".join(DEFAULT_STANDARD_RATES)
    )
    parser.add_argument("--curve-rates", default=",".join(DEFAULT_CURVE_RATES))
    parser.add_argument("--duration", default="30us")
    parser.add_argument("--data-limit", default="0B")
    parser.add_argument("--probe-data-limit", default="0B")
    parser.add_argument("--probe-rate", default="1GiB/s")
    parser.add_argument("--addr-range", default="1GiB")
    parser.add_argument("--probe-addr-range", default="1GiB")
    parser.add_argument("--mem-size", default="16GiB")
    parser.add_argument("--num-generators", type=int, default=1)
    parser.add_argument("--curve-stream-generators", type=int, default=1)
    parser.add_argument(
        "--curve-stream-generator",
        choices=["dram", "linear"],
        default="dram",
    )
    parser.add_argument("--stream-num-seq-pkts", type=int, default=8)
    parser.add_argument("--stream-page-size", default="4KiB")
    parser.add_argument("--stream-banks", type=int, default=32)
    parser.add_argument("--stream-banks-util", type=int, default=32)
    parser.add_argument("--cache-line-size", type=int, default=64)
    parser.add_argument("--mixed-read-percent", type=int, default=50)
    parser.add_argument("--dram-addr-mapping", default="RoCoRaBaCh")
    parser.add_argument("--sys-clock", default="2.4GHz")
    parser.add_argument(
        "--ramulator-python-path",
        default="ext/ramulator2/ramulator2/python",
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--report-only",
        action="store_true",
        help="Skip running gem5; rebuild CSV/JSON/plots/report from the "
        "stats already present in the existing run directories.",
    )
    return parser.parse_args()


def _result_from_existing(
    args: argparse.Namespace,
    backend: str,
    pattern: str,
    rate: str,
) -> Optional[Dict[str, object]]:
    run_id = f"{pattern}-{backend}-{_sanitize(rate)}"
    run_dir = args.outdir / "runs" / run_id
    if not (run_dir / "stats.txt").exists():
        return None
    metadata = _load_metadata(run_dir / "run_metadata.json")
    success = bool(metadata) or (run_dir / "stats.txt").exists()
    return _result_from_run(
        args, run_id, backend, pattern, rate, run_dir, success, 0.0
    )


def main() -> None:
    args = _parse_arguments()
    args.outdir.mkdir(parents=True, exist_ok=True)

    backends = _split_list(args.backends)
    standard_patterns = _split_list(args.standard_patterns)
    curve_patterns = _split_list(args.curve_patterns)
    standard_rates = _split_list(args.standard_rates)
    curve_rates = _split_list(args.curve_rates)

    results: List[Dict[str, object]] = []
    for backend in backends:
        for pattern in standard_patterns:
            for rate in standard_rates:
                if args.report_only:
                    row = _result_from_existing(args, backend, pattern, rate)
                    if row is not None:
                        results.append(row)
                else:
                    results.append(_run_one(args, backend, pattern, rate))
        for pattern in curve_patterns:
            for rate in curve_rates:
                if args.report_only:
                    row = _result_from_existing(args, backend, pattern, rate)
                    if row is not None:
                        results.append(row)
                else:
                    results.append(_run_one(args, backend, pattern, rate))

    _write_csv(results, args.outdir / "results.csv")
    (args.outdir / "results.json").write_text(
        json.dumps(_json_safe(results), indent=2, allow_nan=False) + "\n"
    )
    plots = _write_plots(results, args.outdir / "plots")
    _write_report(args, results, plots)

    failures = [row for row in results if not row.get("success")]
    if failures:
        print(f"{len(failures)} runs failed; see {args.outdir}")
    else:
        print(f"Wrote {args.outdir / 'report.md'}")


if __name__ == "__main__":
    main()
