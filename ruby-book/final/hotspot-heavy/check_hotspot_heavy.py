#!/usr/bin/env python3
"""
check_hotspot_heavy.py -- Analyse the single-thread hotspot-heavy run.

Usage:
    python3 check_hotspot_heavy.py <m5out-dir> <console-log>
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass

PREFIX = "RBOOK_HOTSPOT_HEAVY"
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
    def hnf15_accesses_per_cycle(self) -> float:
        return self.hnf15_accesses / float(self.console.cycles)

    @property
    def ext_flits_per_op(self) -> float:
        return self.hnf15_ext_flits / float(self.console.ops)

    @property
    def ext_flits_per_cycle(self) -> float:
        return self.hnf15_ext_flits / float(self.console.cycles)

    @property
    def net_flits_per_cycle(self) -> float:
        return self.flits_received_total / float(self.console.cycles)

    @property
    def router15_reads_per_cycle(self) -> float:
        return self.router15_buffer_reads / float(self.console.cycles)


@dataclass
class AnalysisResult:
    m5out_dir: str
    console_path: str
    command_line: str
    gem5_started: str
    saw_pass: bool
    stream_pages: int
    read_streams: int
    write_streams: int
    hot_lines_per_page: int
    hot_offset: str
    main_cpu: int
    stats_block_count: int
    window: StatsWindow
    errors: list[str]

    @property
    def passed(self) -> bool:
        return not self.errors

    @property
    def line_ops_per_page(self) -> int:
        return self.hot_lines_per_page * (
            self.read_streams + self.write_streams
        )


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

    if not blocks:
        fail(f"expected at least one dumped stats block in {stats_path}")

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
        if section.startswith("system.ruby.network.ext_links") and (
            values.get("ext_node") == ext_node
        ):
            return section
    fail(f"could not find ext link for {ext_node}")


def inbound_int_links(
    sections: dict[str, dict[str, str]], router: str
) -> list[str]:
    result = []

    for section, values in sections.items():
        if section.startswith("system.ruby.network.int_links") and (
            values.get("dst_node") == router
        ):
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
) -> tuple[dict[str, str], ConsoleWindow, dict[str, str], bool]:
    metadata = {"command_line": "", "gem5_started": ""}
    scalar_metrics: dict[str, str] = {}
    window: ConsoleWindow | None = None
    saw_pass = False
    scalar_pattern = re.compile(
        rf"^{PREFIX}\s+"
        r"(MAIN_CPU|STREAM_PAGES|READ_STREAMS|WRITE_STREAMS|HOT_OFFSET|HOT_LINES_PER_PAGE)\s+"
        r"(.+?)\s*$"
    )
    window_pattern = re.compile(
        rf"^{PREFIX}\s+WINDOW\s+(\d+)\s+THREADS\s+(\d+)\s+OPS\s+(\d+)\s+"
        r"CYCLES\s+(\d+)\s+THROUGHPUT\s+([-+]?\d+(?:\.\d+)?)\s+COMPLETED\s+(\d+)\s*$"
    )

    with open(console_path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.rstrip("\n")

            match = scalar_pattern.match(line)
            if match:
                scalar_metrics[match.group(1)] = match.group(2)
                continue

            match = window_pattern.match(line)
            if match:
                window = ConsoleWindow(
                    index=int(match.group(1)),
                    threads=int(match.group(2)),
                    ops=int(match.group(3)),
                    cycles=int(match.group(4)),
                    throughput=float(match.group(5)),
                    completed=int(match.group(6)),
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
        "READ_STREAMS",
        "WRITE_STREAMS",
        "HOT_OFFSET",
        "HOT_LINES_PER_PAGE",
    ):
        if key not in scalar_metrics:
            fail(f"missing {key} in {console_path}")

    if window is None:
        fail(f"missing measured window in {console_path}")

    return scalar_metrics, window, metadata, saw_pass


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

    scalar_metrics, console_window, metadata, saw_pass = parse_console(
        console_path
    )
    blocks = parse_stats_blocks(stats_path)
    sections = parse_config_sections(config_path)
    hnf15_ext_link = find_ext_link(sections, HNF15_EXT_NODE)
    router15_inbound_links = inbound_int_links(sections, ROUTER15)
    window = build_stats_window(
        blocks[0], console_window, hnf15_ext_link, router15_inbound_links
    )

    stream_pages = int(scalar_metrics["STREAM_PAGES"], 0)
    read_streams = int(scalar_metrics["READ_STREAMS"], 0)
    write_streams = int(scalar_metrics["WRITE_STREAMS"], 0)
    hot_lines_per_page = int(scalar_metrics["HOT_LINES_PER_PAGE"], 0)
    main_cpu = int(scalar_metrics["MAIN_CPU"], 0)
    expected_ops = (
        stream_pages * hot_lines_per_page * (read_streams + write_streams)
    )
    errors: list[str] = []

    if not saw_pass:
        errors.append("benchmark did not print PASS")
    if main_cpu != 0:
        errors.append(f"benchmark reported MAIN_CPU={main_cpu} instead of 0")
    if read_streams + write_streams <= 0:
        errors.append("benchmark reported zero total streams")
    if window.console.index != 0:
        errors.append(
            f"window index mismatch: expected 0, got {window.console.index}"
        )
    if window.console.threads != 1:
        errors.append(
            f"window used {window.console.threads} threads instead of 1"
        )
    if window.console.completed != 1:
        errors.append(
            f"window completed {window.console.completed} threads instead of 1"
        )
    if window.console.ops != expected_ops:
        errors.append(
            f"window reported {window.console.ops} ops instead of {expected_ops}"
        )
    if window.console.cycles <= 0:
        errors.append("window reported non-positive cycle count")
    if window.console.throughput <= 0.0:
        errors.append("window reported non-positive throughput")
    if window.hnf15_accesses <= 0:
        errors.append("window has no HNF15 demand accesses")
    if window.hnf15_accesses <= window.hnf15_other_max * 8.0:
        errors.append(
            f"HNF15 accesses ({window.hnf15_accesses:.0f}) do not dominate HNF{window.hnf15_other_idx} ({window.hnf15_other_max:.0f})"
        )
    if window.flits_received_total <= 0:
        errors.append("window has no received flits")
    if window.packets_received_total <= 0:
        errors.append("window has no received packets")
    if window.hnf15_ext_flits <= 0:
        errors.append("window has no HNF15 ext-link flits")
    if window.router15_buffer_reads <= 0:
        errors.append("window has no router15 buffer reads")
    if window.router15_buffer_writes <= 0:
        errors.append("window has no router15 buffer writes")
    if window.inbound_sum <= 0:
        errors.append("window has no inbound router15 traffic")

    return AnalysisResult(
        m5out_dir=m5out_dir,
        console_path=console_path,
        command_line=metadata.get("command_line", ""),
        gem5_started=metadata.get("gem5_started", ""),
        saw_pass=saw_pass,
        stream_pages=stream_pages,
        read_streams=read_streams,
        write_streams=write_streams,
        hot_lines_per_page=hot_lines_per_page,
        hot_offset=scalar_metrics["HOT_OFFSET"],
        main_cpu=main_cpu,
        stats_block_count=len(blocks),
        window=window,
        errors=errors,
    )


def format_markdown_report(result: AnalysisResult) -> str:
    window = result.window
    lines = [
        "# Hotspot Heavy Summary",
        "",
        "## Result",
        "",
        f"- Status: `{'PASS' if result.passed else 'FAIL'}`",
        f"- m5out directory: `{result.m5out_dir}`",
        f"- Console log: `{result.console_path}`",
        f"- Stats blocks observed: `{result.stats_block_count}` "
        "(report uses the first dumped window)",
        f"- Stream pages: `{result.stream_pages}`",
        f"- Mapped space per stream: `{result.stream_pages * PAGE_BYTES // (1024 * 1024)} MiB`",
        f"- Read streams: `{result.read_streams}`",
        f"- Write streams: `{result.write_streams}`",
        f"- Targeted HNF15 lines per page across all streams: `{result.line_ops_per_page}`",
        f"- Hot line offset: `{result.hot_offset}`",
        f"- Main CPU: `{result.main_cpu}`",
    ]

    if result.gem5_started:
        lines.append(f"- {result.gem5_started}")
    if result.command_line:
        lines.append(f"- Command line: `{result.command_line}`")

    lines.extend(
        [
            "",
            "## Measured Window",
            "",
            "| Threads | Ops | Cycles | Line ops/cycle | HNF15 accesses | HNF15 accesses/cycle | HNF15 / ops | Net flits/cycle | HNF15 ext flits/cycle | HNF15 ext flits/op | Router15 reads/cycle | Avg flit queueing | Avg flit network |",
            "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
            f"| {window.console.threads} | {window.console.ops} | {window.console.cycles} | {window.console.throughput:.6f} | {window.hnf15_accesses:.0f} | {window.hnf15_accesses_per_cycle:.6f} | {window.hnf15_ops_ratio:.3f} | {window.net_flits_per_cycle:.3f} | {window.ext_flits_per_cycle:.3f} | {window.ext_flits_per_op:.3f} | {window.router15_reads_per_cycle:.3f} | {window.queueing_latency:.2f} | {window.network_latency:.2f} |",
        ]
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
    print(format_markdown_report(result), end="")
    if not result.passed:
        sys.exit(1)


if __name__ == "__main__":
    main()
