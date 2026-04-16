#!/usr/bin/env python3
"""
check_hotspot.py -- Analyse Stage 3f hotspot output and stats.

Usage:
    python3 check_hotspot.py <m5out-dir> <console-log>
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass

EXPECTED_THREADS = [1, 2, 4, 8, 16]
HNF15_EXT_NODE = "system.ruby.hnf15.cntrl"
ROUTER15 = "system.ruby.network.routers15"
PAGE_BYTES = 4096


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
    hnf15_accesses: float
    hnf15_other_max: float
    hnf15_other_idx: int
    router15_buffer_reads: float
    router15_buffer_writes: float
    hnf15_ext_flits: float
    inbound_to_router15: list[tuple[str, float]]

    @property
    def inbound_sum(self) -> float:
        return sum(value for _, value in self.inbound_to_router15)

    @property
    def hnf15_ops_ratio(self) -> float:
        return self.hnf15_accesses / float(self.console.ops)

    @property
    def ext_flits_per_op(self) -> float:
        return self.hnf15_ext_flits / float(self.console.ops)

    @property
    def router15_reads_per_op(self) -> float:
        return self.router15_buffer_reads / float(self.console.ops)


@dataclass
class AnalysisResult:
    m5out_dir: str
    console_path: str
    command_line: str
    gem5_started: str
    saw_pass: bool
    stream_pages: int
    hot_lines_per_page: int
    hot_offset: str
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

            match = scalar.match(line)
            if match:
                current[match.group(1)] = float(match.group(2))

    if len(blocks) < len(EXPECTED_THREADS):
        fail(
            f"expected at least {len(EXPECTED_THREADS)} dumped stats blocks in {stats_path}"
        )

    return blocks


def parse_config_sections(config_path: str) -> dict[str, dict[str, str]]:
    sections: dict[str, dict[str, str]] = {}
    current = None

    with open(config_path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if not line:
                continue
            if line.startswith("[") and line.endswith("]"):
                current = line[1:-1]
                sections[current] = {}
                continue
            if current is None or "=" not in line:
                continue

            key, value = line.split("=", 1)
            sections[current][key.strip()] = value.strip()

    return sections


def find_ext_link(sections: dict[str, dict[str, str]], ext_node: str) -> str:
    for section, values in sections.items():
        if not section.startswith("system.ruby.network.ext_links"):
            continue
        if values.get("ext_node") == ext_node:
            return section
    fail(f"could not find ext link for {ext_node}")


def inbound_int_links(
    sections: dict[str, dict[str, str]], router: str
) -> list[str]:
    result = []

    for section, values in sections.items():
        if not section.startswith("system.ruby.network.int_links"):
            continue
        if values.get("dst_node") == router:
            result.append(section)

    if not result:
        fail(f"could not find inbound int links for {router}")

    return sorted(result)


def stat_value(block: dict[str, float], name: str) -> float:
    return block.get(name, 0.0)


def ext_link_total(block: dict[str, float], section: str) -> float:
    return stat_value(
        block, f"{section}.network_links0.flits_per_vnet::total"
    ) + stat_value(block, f"{section}.network_links1.flits_per_vnet::total")


def int_link_total(block: dict[str, float], section: str) -> float:
    return stat_value(block, f"{section}.network_link.flits_per_vnet::total")


def parse_console(
    console_path: str,
) -> tuple[
    dict[str, str], list[int], list[ConsoleWindow], dict[str, str], bool
]:
    metadata = {"command_line": "", "gem5_started": ""}
    scalar_metrics: dict[str, str] = {}
    worker_cpus: dict[int, int] = {}
    windows: list[ConsoleWindow] = []
    saw_pass = False
    scalar_pattern = re.compile(
        r"^RBOOK_HOTSPOT\s+"
        r"(MAIN_CPU|STREAM_PAGES|HOT_OFFSET|HOT_LINES_PER_PAGE)\s+"
        r"(.+?)\s*$"
    )
    worker_pattern = re.compile(
        r"^RBOOK_HOTSPOT\s+WORKER_(\d+)_CPU\s+(-?\d+)\s*$"
    )
    window_pattern = re.compile(
        r"^RBOOK_HOTSPOT\s+WINDOW\s+(\d+)\s+THREADS\s+(\d+)\s+OPS\s+(\d+)\s+"
        r"CYCLES\s+(\d+)\s+THROUGHPUT\s+([-+]?\d+(?:\.\d+)?)\s+COMPLETED\s+(\d+)\s*$"
    )

    with open(console_path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.rstrip("\n")

            match = scalar_pattern.match(line)
            if match:
                scalar_metrics[match.group(1)] = match.group(2)
                continue

            match = worker_pattern.match(line)
            if match:
                worker_cpus[int(match.group(1))] = int(match.group(2))
                continue

            match = window_pattern.match(line)
            if match:
                windows.append(
                    ConsoleWindow(
                        index=int(match.group(1)),
                        threads=int(match.group(2)),
                        ops=int(match.group(3)),
                        cycles=int(match.group(4)),
                        throughput=float(match.group(5)),
                        completed=int(match.group(6)),
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

    for key in (
        "MAIN_CPU",
        "STREAM_PAGES",
        "HOT_OFFSET",
        "HOT_LINES_PER_PAGE",
    ):
        if key not in scalar_metrics:
            fail(f"missing {key} in {console_path}")

    ordered_workers = [worker_cpus.get(i, -1) for i in range(1, 16)]
    if any(cpu < 0 for cpu in ordered_workers):
        fail(f"missing worker CPU records in {console_path}")

    if len(windows) != len(EXPECTED_THREADS):
        fail(
            f"expected {len(EXPECTED_THREADS)} hotspot windows, found {len(windows)}"
        )

    return scalar_metrics, ordered_workers, windows, metadata, saw_pass


def build_stats_window(
    block: dict[str, float],
    console: ConsoleWindow,
    hnf15_ext_link: str,
    router15_inbound_links: list[str],
) -> StatsWindow:
    hnf_accesses = [
        stat_value(block, f"system.ruby.hnf{i}.cntrl.cache.m_demand_accesses")
        for i in range(16)
    ]
    other_pairs = [
        (i, value) for i, value in enumerate(hnf_accesses) if i != 15
    ]
    other_idx, other_max = max(other_pairs, key=lambda item: item[1])

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
        hnf15_accesses=hnf_accesses[15],
        hnf15_other_max=other_max,
        hnf15_other_idx=other_idx,
        router15_buffer_reads=stat_value(block, f"{ROUTER15}.buffer_reads"),
        router15_buffer_writes=stat_value(block, f"{ROUTER15}.buffer_writes"),
        hnf15_ext_flits=ext_link_total(block, hnf15_ext_link),
        inbound_to_router15=[
            (section, int_link_total(block, section))
            for section in router15_inbound_links
        ],
    )


def analyze_run(m5out_dir: str, console_path: str) -> AnalysisResult:
    stats_path = os.path.join(m5out_dir, "stats.txt")
    config_path = os.path.join(m5out_dir, "config.ini")

    for path in (console_path, stats_path, config_path):
        if not os.path.isfile(path):
            fail(f"missing required file: {path}")

    scalar_metrics, worker_cpus, console_windows, metadata, saw_pass = (
        parse_console(console_path)
    )
    blocks = parse_stats_blocks(stats_path)
    sections = parse_config_sections(config_path)
    hnf15_ext_link = find_ext_link(sections, HNF15_EXT_NODE)
    router15_inbound_links = inbound_int_links(sections, ROUTER15)

    windows = [
        build_stats_window(
            blocks[i],
            console_windows[i],
            hnf15_ext_link,
            router15_inbound_links,
        )
        for i in range(len(EXPECTED_THREADS))
    ]

    stream_pages = int(scalar_metrics["STREAM_PAGES"], 0)
    hot_lines_per_page = int(scalar_metrics["HOT_LINES_PER_PAGE"], 0)
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
    prev_reads = -1.0
    prev_writes = -1.0
    prev_ext = -1.0
    prev_packets = -1.0
    for idx, window in enumerate(windows):
        expected_threads = EXPECTED_THREADS[idx]
        expected_ops = expected_threads * stream_pages * hot_lines_per_page

        if window.console.index != idx:
            errors.append(
                f"window index mismatch: expected {idx}, got {window.console.index}"
            )
        if window.console.threads != expected_threads:
            errors.append(
                f"window {idx} used {window.console.threads} threads instead of {expected_threads}"
            )
        if window.console.completed != expected_threads:
            errors.append(
                f"window {idx} completed {window.console.completed} threads instead of {expected_threads}"
            )
        if window.console.ops != expected_ops:
            errors.append(
                f"window {idx} reported {window.console.ops} ops instead of {expected_ops}"
            )
        if window.console.cycles <= 0:
            errors.append(f"window {idx} reported non-positive cycle count")
        if window.console.throughput <= 0.0:
            errors.append(f"window {idx} reported non-positive throughput")

        if window.hnf15_accesses <= 0:
            errors.append(f"window {idx} has no HNF15 demand accesses")
        if window.hnf15_accesses < expected_ops * 0.95:
            errors.append(
                f"window {idx} HNF15 accesses ({window.hnf15_accesses:.0f}) are below 95% of expected ops ({expected_ops})"
            )
        if window.hnf15_accesses > expected_ops * 1.10:
            errors.append(
                f"window {idx} HNF15 accesses ({window.hnf15_accesses:.0f}) are above 110% of expected ops ({expected_ops})"
            )
        if window.flits_received_total <= 0:
            errors.append(f"window {idx} has no received flits")
        if window.packets_received_total <= 0:
            errors.append(f"window {idx} has no received packets")
        if window.hnf15_ext_flits <= 0:
            errors.append(f"window {idx} has no HNF15 ext-link flits")
        if window.router15_buffer_reads <= 0:
            errors.append(f"window {idx} has no router15 buffer reads")
        if window.router15_buffer_writes <= 0:
            errors.append(f"window {idx} has no router15 buffer writes")
        if window.inbound_sum <= 0:
            errors.append(f"window {idx} has no inbound router15 traffic")

        if idx > 0:
            if window.flits_received_total <= prev_flits:
                errors.append(
                    f"window {idx} flits ({window.flits_received_total:.0f}) did not grow over window {idx - 1} ({prev_flits:.0f})"
                )
            if window.packets_received_total <= prev_packets:
                errors.append(
                    f"window {idx} packets ({window.packets_received_total:.0f}) did not grow over window {idx - 1} ({prev_packets:.0f})"
                )
            if window.router15_buffer_reads <= prev_reads:
                errors.append(
                    f"window {idx} router15 buffer reads ({window.router15_buffer_reads:.0f}) did not grow over window {idx - 1} ({prev_reads:.0f})"
                )
            if window.router15_buffer_writes <= prev_writes:
                errors.append(
                    f"window {idx} router15 buffer writes ({window.router15_buffer_writes:.0f}) did not grow over window {idx - 1} ({prev_writes:.0f})"
                )
            if window.hnf15_ext_flits <= prev_ext:
                errors.append(
                    f"window {idx} HNF15 ext-link flits ({window.hnf15_ext_flits:.0f}) did not grow over window {idx - 1} ({prev_ext:.0f})"
                )

        prev_flits = window.flits_received_total
        prev_packets = window.packets_received_total
        prev_reads = window.router15_buffer_reads
        prev_writes = window.router15_buffer_writes
        prev_ext = window.hnf15_ext_flits

    low = windows[0]
    mid = windows[3]
    high = windows[-1]
    if high.queueing_latency <= mid.queueing_latency:
        errors.append(
            "16-thread queueing latency does not exceed the 8-thread window"
        )
    if high.ext_flits_per_op <= low.ext_flits_per_op:
        errors.append(
            "16-thread HNF15 ext-link flits per op do not exceed the 1-thread window"
        )
    if high.router15_reads_per_op <= low.router15_reads_per_op:
        errors.append(
            "16-thread router15 buffer reads per op do not exceed the 1-thread window"
        )

    low_eff = low.console.throughput / low.console.threads
    high_eff = high.console.throughput / high.console.threads
    if high_eff >= low_eff * 0.80:
        errors.append(
            "16-thread throughput efficiency is not at least 20% lower than the 1-thread window"
        )

    return AnalysisResult(
        m5out_dir=m5out_dir,
        console_path=console_path,
        command_line=metadata.get("command_line", ""),
        gem5_started=metadata.get("gem5_started", ""),
        saw_pass=saw_pass,
        stream_pages=stream_pages,
        hot_lines_per_page=hot_lines_per_page,
        hot_offset=scalar_metrics["HOT_OFFSET"],
        main_cpu=main_cpu,
        worker_cpus=worker_cpus,
        stats_block_count=len(blocks),
        windows=windows,
        errors=errors,
    )


def format_text_report(result: AnalysisResult) -> str:
    lines = [
        f"Stream pages per active CPU: {result.stream_pages}",
        f"Measured working set per active CPU: {result.stream_pages * PAGE_BYTES // (1024 * 1024)} MiB",
        f"HNF15 lines touched per page: {result.hot_lines_per_page}",
        f"Hot offset: {result.hot_offset}",
        "",
        "Threads  Ops    Cycles    Thrpt    Qlat    Nlat   HNF15  Ratio  Ext/op  R15R/op",
        "-------  -----  --------  -------  ------  -----  -----  -----  ------  -------",
    ]

    for window in result.windows:
        lines.append(
            f"{window.console.threads:>7}  {window.console.ops:>5}  "
            f"{window.console.cycles:>8}  {window.console.throughput:>7.4f}  "
            f"{window.queueing_latency:>6.2f}  {window.network_latency:>5.2f}  "
            f"{window.hnf15_accesses:>5.0f}  {window.hnf15_ops_ratio:>5.3f}  "
            f"{window.ext_flits_per_op:>6.2f}  "
            f"{window.router15_reads_per_op:>7.2f}"
        )

    if result.errors:
        lines.extend(["", "FAIL"])
        lines.extend(f"  - {error}" for error in result.errors)
    else:
        lines.extend(
            [
                "",
                "PASS -- HNF15 accesses track the intended ops, destination-side traffic per op rises, and per-thread throughput efficiency falls",
            ]
        )

    return "\n".join(lines)


def format_markdown_report(result: AnalysisResult) -> str:
    lines = [
        "# Hotspot Backpressure Summary",
        "",
        "## Result",
        "",
        f"- Status: `{'PASS' if result.passed else 'FAIL'}`",
        f"- m5out directory: `{result.m5out_dir}`",
        f"- Console log: `{result.console_path}`",
        f"- Stats blocks observed: `{result.stats_block_count}` "
        f"(checker uses the first `{len(EXPECTED_THREADS)}` dumped windows)",
        f"- Stream pages per active CPU: `{result.stream_pages}`",
        f"- Measured working set per active CPU: `{result.stream_pages * PAGE_BYTES // (1024 * 1024)} MiB`",
        f"- HNF15 lines touched per page: `{result.hot_lines_per_page}`",
        f"- Hot line offset: `{result.hot_offset}`",
        f"- Main CPU: `{result.main_cpu}`",
        f"- Worker CPUs: `{', '.join(str(cpu) for cpu in result.worker_cpus)}`",
    ]

    if result.gem5_started:
        lines.append(f"- {result.gem5_started}")
    if result.command_line:
        lines.append(f"- Command line: `{result.command_line}`")

    lines.extend(
        [
            "",
            "## Window Sweep",
            "",
            "| Threads | Ops | Cycles | Throughput (ops/cycle) | Avg flit queueing | Avg flit network | HNF15 accesses | HNF15 / ops | HNF15 ext-link flits | Router15 buffer reads | Router15 buffer writes | Inbound router15 flits |",
            "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )

    for window in result.windows:
        lines.append(
            f"| {window.console.threads} | {window.console.ops} | {window.console.cycles} | {window.console.throughput:.6f} | {window.queueing_latency:.2f} | {window.network_latency:.2f} | {window.hnf15_accesses:.0f} | {window.hnf15_ops_ratio:.3f} | {window.hnf15_ext_flits:.0f} | {window.router15_buffer_reads:.0f} | {window.router15_buffer_writes:.0f} | {window.inbound_sum:.0f} |"
        )

    lines.extend(["", "## Interpretation", ""])
    if (
        result.windows[-1].queueing_latency
        >= result.windows[0].queueing_latency
    ):
        lines.append(
            f"- The queueing latency rises from `{result.windows[0].queueing_latency:.2f}` at 1 thread to `{result.windows[-1].queueing_latency:.2f}` at 16 threads."
        )
    else:
        lines.append(
            f"- The queueing latency stays elevated across the sweep, with `{result.windows[0].queueing_latency:.2f}` at 1 thread and `{result.windows[-1].queueing_latency:.2f}` at 16 threads."
        )
    lines.append(
        f"- The HNF15 ext-link flits rise from `{result.windows[0].hnf15_ext_flits:.0f}` to `{result.windows[-1].hnf15_ext_flits:.0f}` across the sweep."
    )
    lines.append(
        f"- Destination pressure per requested line rises from `{result.windows[0].ext_flits_per_op:.2f}` to `{result.windows[-1].ext_flits_per_op:.2f}` HNF15 ext-link flits/op."
    )
    lines.append(
        f"- HNF15 demand accesses track the intended data-stream ops closely, from `{result.windows[0].hnf15_ops_ratio:.3f}` to `{result.windows[-1].hnf15_ops_ratio:.3f}` accesses per requested line."
    )
    lines.append(
        f"- Per-thread throughput efficiency falls from `{result.windows[0].console.throughput / result.windows[0].console.threads:.6f}` to `{result.windows[-1].console.throughput / result.windows[-1].console.threads:.6f}` ops/cycle/thread."
    )
    lines.append(
        "- Each measured line is private to one CPU but shares the same HNF15 home-node offset, so the traffic increase reflects many-to-one pressure rather than false sharing."
    )

    lines.extend(["", "## Inbound Router15 Links", ""])
    lines.append("| Threads | Link | Flits |")
    lines.append("| ---: | --- | ---: |")
    for window in result.windows:
        for link, value in window.inbound_to_router15:
            short = link.split(".")[-1]
            lines.append(
                f"| {window.console.threads} | `{short}` | {value:.0f} |"
            )

    if result.errors:
        lines.extend(["", "## Failures", ""])
        for error in result.errors:
            lines.append(f"- {error}")

    lines.append("")
    return "\n".join(lines)


def main() -> None:
    if len(sys.argv) != 3:
        print(
            f"Usage: {sys.argv[0]} <m5out-dir> <console-log>",
            file=sys.stderr,
        )
        sys.exit(1)

    result = analyze_run(sys.argv[1], sys.argv[2])
    print(format_text_report(result), end="\n")
    if not result.passed:
        sys.exit(1)


if __name__ == "__main__":
    main()
