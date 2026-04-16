#!/usr/bin/env python3
"""
check_link_pressure.py -- Validate output of the link-pressure benchmark.

Usage:
    python3 check_link_pressure.py <m5out-dir> <console-log>

Pass criteria:
  - Benchmark prints PASS.
  - 5 windows (1, 2, 4, 8, 16 threads) report positive ops/cycles.
  - Every HNF (0..15) sees > 0 demand accesses, confirming the traffic
    is spread across the mesh (many-to-many) rather than concentrated
    on one home node.
  - max/min HNF load ratio is under 2x at 16 threads.
  - Total flits received grows monotonically with thread count.
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass

EXPECTED_THREADS = [1, 2, 4, 8, 16]


@dataclass
class ConsoleWindow:
    index: int
    threads: int
    ops: int
    cycles: int
    throughput: float
    completed: int


@dataclass
class StatsWindow:
    console: ConsoleWindow
    queueing_latency: float
    network_latency: float
    flits_received_total: float
    packets_received_total: float
    hnf_accesses: list[float]

    @property
    def hnf_max(self) -> float:
        return max(self.hnf_accesses)

    @property
    def hnf_min(self) -> float:
        return min(self.hnf_accesses)

    @property
    def hnf_mean(self) -> float:
        return sum(self.hnf_accesses) / len(self.hnf_accesses)

    @property
    def hnf_spread(self) -> float:
        return (
            self.hnf_max / self.hnf_min if self.hnf_min > 0 else float("inf")
        )


@dataclass
class AnalysisResult:
    m5out_dir: str
    console_path: str
    command_line: str
    gem5_started: str
    saw_pass: bool
    stream_pages: int
    total_lines: int
    buffer_bytes: int
    main_cpu: int
    worker_cpus: list[int]
    stats_block_count: int
    windows: list[StatsWindow]
    errors: list[str]

    @property
    def passed(self) -> bool:
        return not self.errors


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def parse_stats_blocks(stats_path: str) -> list[dict[str, float]]:
    blocks: list[dict[str, float]] = []
    current: dict[str, float] | None = None
    begin_prefix = "---------- Begin Simulation Statistics"
    end_prefix = "---------- End Simulation Statistics"
    scalar = re.compile(r"^(\S+)\s+([-+]?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)\b")

    with open(stats_path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if line.startswith(begin_prefix):
                current = {}
                continue
            if line.startswith(end_prefix):
                if current is not None:
                    blocks.append(current)
                current = None
                continue
            if current is None:
                continue
            m = scalar.match(line)
            if m:
                current[m.group(1)] = float(m.group(2))

    if len(blocks) < len(EXPECTED_THREADS):
        fail(
            f"expected >= {len(EXPECTED_THREADS)} stats blocks in {stats_path}"
        )

    return blocks


def stat_value(block: dict[str, float], name: str) -> float:
    return block.get(name, 0.0)


def parse_console(console_path: str):
    metadata = {"command_line": "", "gem5_started": ""}
    scalar_metrics: dict[str, str] = {}
    worker_cpus: dict[int, int] = {}
    windows: list[ConsoleWindow] = []
    saw_pass = False

    scalar_pattern = re.compile(
        r"^RBOOK_LINK_PRESSURE\s+"
        r"(MAIN_CPU|STREAM_PAGES|TOTAL_LINES|BUFFER_BYTES)\s+(.+?)\s*$"
    )
    worker_pattern = re.compile(
        r"^RBOOK_LINK_PRESSURE\s+WORKER_(\d+)_CPU\s+(-?\d+)\s*$"
    )
    window_pattern = re.compile(
        r"^RBOOK_LINK_PRESSURE\s+WINDOW\s+(\d+)\s+THREADS\s+(\d+)\s+OPS\s+(\d+)\s+"
        r"CYCLES\s+(\d+)\s+THROUGHPUT\s+([-+]?\d+(?:\.\d+)?)\s+COMPLETED\s+(\d+)\s*$"
    )

    with open(console_path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.rstrip("\n")

            m = scalar_pattern.match(line)
            if m:
                scalar_metrics[m.group(1)] = m.group(2)
                continue
            m = worker_pattern.match(line)
            if m:
                worker_cpus[int(m.group(1))] = int(m.group(2))
                continue
            m = window_pattern.match(line)
            if m:
                windows.append(
                    ConsoleWindow(
                        index=int(m.group(1)),
                        threads=int(m.group(2)),
                        ops=int(m.group(3)),
                        cycles=int(m.group(4)),
                        throughput=float(m.group(5)),
                        completed=int(m.group(6)),
                    )
                )
                continue
            if line == "PASS":
                saw_pass = True
            elif line.startswith("gem5 started "):
                metadata["gem5_started"] = line.strip()
            elif line.startswith("command line: "):
                metadata["command_line"] = line[
                    len("command line: ") :
                ].strip()

    for key in ("MAIN_CPU", "STREAM_PAGES", "TOTAL_LINES", "BUFFER_BYTES"):
        if key not in scalar_metrics:
            fail(f"missing {key} in {console_path}")

    ordered_workers = [worker_cpus.get(i, -1) for i in range(1, 16)]
    if any(cpu < 0 for cpu in ordered_workers):
        fail(f"missing worker CPU records in {console_path}")
    if len(windows) != len(EXPECTED_THREADS):
        fail(f"expected {len(EXPECTED_THREADS)} windows, found {len(windows)}")

    return scalar_metrics, ordered_workers, windows, metadata, saw_pass


def build_stats_window(
    block: dict[str, float], console: ConsoleWindow
) -> StatsWindow:
    hnf_accesses = [
        stat_value(block, f"system.ruby.hnf{i}.cntrl.cache.m_demand_accesses")
        for i in range(16)
    ]
    return StatsWindow(
        console=console,
        queueing_latency=stat_value(
            block, "system.ruby.network.average_flit_queueing_latency"
        ),
        network_latency=stat_value(
            block, "system.ruby.network.average_flit_network_latency"
        ),
        flits_received_total=stat_value(
            block, "system.ruby.network.flits_received::total"
        ),
        packets_received_total=stat_value(
            block, "system.ruby.network.packets_received::total"
        ),
        hnf_accesses=hnf_accesses,
    )


def analyze_run(m5out_dir: str, console_path: str) -> AnalysisResult:
    stats_path = os.path.join(m5out_dir, "stats.txt")
    for path in (console_path, stats_path):
        if not os.path.isfile(path):
            fail(f"missing required file: {path}")

    scalar_metrics, worker_cpus, console_windows, metadata, saw_pass = (
        parse_console(console_path)
    )
    blocks = parse_stats_blocks(stats_path)

    windows = [
        build_stats_window(blocks[i], console_windows[i])
        for i in range(len(EXPECTED_THREADS))
    ]

    stream_pages = int(scalar_metrics["STREAM_PAGES"], 0)
    total_lines = int(scalar_metrics["TOTAL_LINES"], 0)
    buffer_bytes = int(scalar_metrics["BUFFER_BYTES"], 0)
    main_cpu = int(scalar_metrics["MAIN_CPU"], 0)
    errors: list[str] = []

    if not saw_pass:
        errors.append("benchmark did not print PASS")
    if main_cpu != 0:
        errors.append(f"benchmark reported MAIN_CPU={main_cpu} instead of 0")
    for i, cpu in enumerate(worker_cpus, start=1):
        if cpu != i:
            errors.append(f"worker {i} reported CPU {cpu} instead of {i}")

    prev_flits = -1.0
    for idx, window in enumerate(windows):
        expected_threads = EXPECTED_THREADS[idx]
        expected_ops = expected_threads * total_lines

        if window.console.threads != expected_threads:
            errors.append(
                f"window {idx}: threads={window.console.threads} exp={expected_threads}"
            )
        if window.console.completed != expected_threads:
            errors.append(
                f"window {idx}: completed={window.console.completed} exp={expected_threads}"
            )
        if window.console.ops != expected_ops:
            errors.append(
                f"window {idx}: ops={window.console.ops} exp={expected_ops}"
            )
        if window.console.cycles <= 0 or window.console.throughput <= 0:
            errors.append(f"window {idx}: non-positive cycles or throughput")
        if window.flits_received_total <= 0:
            errors.append(f"window {idx}: no received flits")
        if window.hnf_min <= 0:
            errors.append(f"window {idx}: some HNFs saw zero demand accesses")
        if idx > 0 and window.flits_received_total <= prev_flits:
            errors.append(
                f"window {idx}: flits did not grow "
                f"({window.flits_received_total:.0f} vs {prev_flits:.0f})"
            )
        prev_flits = window.flits_received_total

    high = windows[-1]
    if high.hnf_spread > 2.0:
        errors.append(
            f"16-thread HNF load is too skewed "
            f"(max/min = {high.hnf_spread:.2f}, expected <= 2.0)"
        )

    return AnalysisResult(
        m5out_dir=m5out_dir,
        console_path=console_path,
        command_line=metadata["command_line"],
        gem5_started=metadata["gem5_started"],
        saw_pass=saw_pass,
        stream_pages=stream_pages,
        total_lines=total_lines,
        buffer_bytes=buffer_bytes,
        main_cpu=main_cpu,
        worker_cpus=worker_cpus,
        stats_block_count=len(blocks),
        windows=windows,
        errors=errors,
    )


def format_text_report(result: AnalysisResult) -> str:
    lines = [
        f"Stream pages: {result.stream_pages}",
        f"Buffer size: {result.buffer_bytes // (1024 * 1024)} MiB "
        f"({result.total_lines} cache lines)",
        "",
        "Threads  Ops      Cycles     Thrpt     Qlat      Nlat    HNFmax   HNFmin   HNFspread",
        "-------  -------  ---------  --------  --------  ------  -------  -------  ---------",
    ]
    for w in result.windows:
        lines.append(
            f"{w.console.threads:>7}  {w.console.ops:>7}  "
            f"{w.console.cycles:>9}  {w.console.throughput:>8.5f}  "
            f"{w.queueing_latency:>8.2f}  {w.network_latency:>6.2f}  "
            f"{w.hnf_max:>7.0f}  {w.hnf_min:>7.0f}  {w.hnf_spread:>9.3f}"
        )
    if result.errors:
        lines.extend(["", "FAIL"])
        lines.extend(f"  - {e}" for e in result.errors)
    else:
        lines.append("")
        lines.append(
            "PASS -- every HNF sees traffic, load spread < 2x, flits grow with threads"
        )
    return "\n".join(lines)


def main() -> None:
    if len(sys.argv) != 3:
        print(
            f"Usage: {sys.argv[0]} <m5out-dir> <console-log>", file=sys.stderr
        )
        sys.exit(1)

    result = analyze_run(sys.argv[1], sys.argv[2])
    print(format_text_report(result))
    if not result.passed:
        sys.exit(1)


if __name__ == "__main__":
    main()
