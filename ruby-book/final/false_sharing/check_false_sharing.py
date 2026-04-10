#!/usr/bin/env python3
"""
check_false_sharing.py -- Analyse Stage 3c false-sharing output and stats.

Usage:
    python3 check_false_sharing.py <m5out-dir> <console-log>
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass

FORWARD_PATH_EDGES = [
    ("system.ruby.network.routers00", "system.ruby.network.routers01"),
    ("system.ruby.network.routers01", "system.ruby.network.routers02"),
    ("system.ruby.network.routers02", "system.ruby.network.routers03"),
    ("system.ruby.network.routers03", "system.ruby.network.routers07"),
    ("system.ruby.network.routers07", "system.ruby.network.routers11"),
    ("system.ruby.network.routers11", "system.ruby.network.routers15"),
]

REVERSE_PATH_EDGES = [
    ("system.ruby.network.routers15", "system.ruby.network.routers11"),
    ("system.ruby.network.routers11", "system.ruby.network.routers07"),
    ("system.ruby.network.routers07", "system.ruby.network.routers03"),
    ("system.ruby.network.routers03", "system.ruby.network.routers02"),
    ("system.ruby.network.routers02", "system.ruby.network.routers01"),
    ("system.ruby.network.routers01", "system.ruby.network.routers00"),
]


@dataclass
class WindowMetrics:
    name: str
    cpu0_l1d_accesses: float
    cpu0_l1d_misses: float
    cpu15_l1d_accesses: float
    cpu15_l1d_misses: float
    read_unique_total: float
    store_total: float
    comp_ack_total: float
    send_comp_ack_total: float
    forward_path: list[tuple[str, float]]
    reverse_path: list[tuple[str, float]]

    @property
    def forward_sum(self) -> float:
        return sum(value for _, value in self.forward_path)

    @property
    def reverse_sum(self) -> float:
        return sum(value for _, value in self.reverse_path)


@dataclass
class AnalysisResult:
    m5out_dir: str
    console_path: str
    command_line: str
    gem5_started: str
    iterations: int
    shared_offset: str
    cpu0: int
    cpu15: int
    control_avg: float
    control_min: int
    control_max: int
    false_avg: float
    false_min: int
    false_max: int
    delta_avg: float
    control_value0: int
    control_value1: int
    value0: int
    value1: int
    stats_block_count: int
    control_window: WindowMetrics
    false_window: WindowMetrics
    errors: list[str]

    @property
    def passed(self) -> bool:
        return not self.errors


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def parse_console(
    console_path: str,
) -> tuple[dict[str, str], dict[str, str], bool]:
    metrics: dict[str, str] = {}
    metadata: dict[str, str] = {"command_line": "", "gem5_started": ""}
    saw_pass = False
    pattern = re.compile(r"^RBOOK_FALSE\s+(\S+)\s+(.+?)\s*$")

    with open(console_path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            match = pattern.match(line)
            if match:
                metrics[match.group(1)] = match.group(2)
                continue

            if line == "PASS":
                saw_pass = True
            elif line.startswith("gem5 started "):
                metadata["gem5_started"] = line.strip()
            elif line.startswith("command line: "):
                metadata["command_line"] = line[
                    len("command line: ") :
                ].strip()

    required = (
        "ITERATIONS",
        "CPU0",
        "CPU15",
        "CONTROL_AVG",
        "FALSE_AVG",
        "DELTA_AVG",
        "CONTROL_VALUE0",
        "CONTROL_VALUE1",
        "VALUE0",
        "VALUE1",
    )
    for key in required:
        if key not in metrics:
            fail(f"missing {key} in {console_path}")

    return metrics, metadata, saw_pass


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

    if len(blocks) < 2:
        fail(f"expected at least 2 dumped stats blocks in {stats_path}")

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


def find_int_link(
    sections: dict[str, dict[str, str]], src_node: str, dst_node: str
) -> str:
    for section, values in sections.items():
        if not section.startswith("system.ruby.network.int_links"):
            continue
        if (
            values.get("src_node") == src_node
            and values.get("dst_node") == dst_node
        ):
            return section
    fail(f"could not find int link for {src_node} -> {dst_node}")


def stat_value(block: dict[str, float], name: str) -> float:
    return block.get(name, 0.0)


def int_link_total(block: dict[str, float], section: str) -> float:
    return stat_value(block, f"{section}.network_link.flits_per_vnet::total")


def parse_int_metric(metrics: dict[str, str], key: str) -> int:
    return int(metrics[key], 0)


def parse_float_metric(metrics: dict[str, str], key: str) -> float:
    return float(metrics[key])


def extract_window(
    name: str,
    block: dict[str, float],
    sections: dict[str, dict[str, str]],
) -> WindowMetrics:
    forward_links = [
        find_int_link(sections, src, dst) for src, dst in FORWARD_PATH_EDGES
    ]
    reverse_links = [
        find_int_link(sections, src, dst) for src, dst in REVERSE_PATH_EDGES
    ]

    return WindowMetrics(
        name=name,
        cpu0_l1d_accesses=stat_value(
            block, "system.cpu0.l1d.cache.m_demand_accesses"
        ),
        cpu0_l1d_misses=stat_value(
            block, "system.cpu0.l1d.cache.m_demand_misses"
        ),
        cpu15_l1d_accesses=stat_value(
            block, "system.cpu15.l1d.cache.m_demand_accesses"
        ),
        cpu15_l1d_misses=stat_value(
            block, "system.cpu15.l1d.cache.m_demand_misses"
        ),
        read_unique_total=stat_value(
            block, "system.ruby.Cache_Controller.ReadUnique::total"
        ),
        store_total=stat_value(
            block, "system.ruby.Cache_Controller.Store::total"
        ),
        comp_ack_total=stat_value(
            block, "system.ruby.Cache_Controller.CompAck::total"
        ),
        send_comp_ack_total=stat_value(
            block, "system.ruby.Cache_Controller.SendCompAck::total"
        ),
        forward_path=[
            (link, int_link_total(block, link)) for link in forward_links
        ],
        reverse_path=[
            (link, int_link_total(block, link)) for link in reverse_links
        ],
    )


def analyze_run(m5out_dir: str, console_path: str) -> AnalysisResult:
    stats_path = os.path.join(m5out_dir, "stats.txt")
    config_path = os.path.join(m5out_dir, "config.ini")

    for path in (console_path, stats_path, config_path):
        if not os.path.isfile(path):
            fail(f"missing required file: {path}")

    metrics, metadata, saw_pass = parse_console(console_path)
    blocks = parse_stats_blocks(stats_path)
    sections = parse_config_sections(config_path)
    control_window = extract_window("control", blocks[0], sections)
    false_window = extract_window("false", blocks[1], sections)

    iterations = parse_int_metric(metrics, "ITERATIONS")
    control_avg = parse_float_metric(metrics, "CONTROL_AVG")
    false_avg = parse_float_metric(metrics, "FALSE_AVG")
    delta_avg = parse_float_metric(metrics, "DELTA_AVG")
    control_value0 = parse_int_metric(metrics, "CONTROL_VALUE0")
    control_value1 = parse_int_metric(metrics, "CONTROL_VALUE1")
    value0 = parse_int_metric(metrics, "VALUE0")
    value1 = parse_int_metric(metrics, "VALUE1")

    errors: list[str] = []

    if not saw_pass:
        errors.append("benchmark did not print PASS")
    if parse_int_metric(metrics, "CPU0") != 0:
        errors.append(
            f"benchmark reported CPU0={metrics['CPU0']} instead of 0"
        )
    if parse_int_metric(metrics, "CPU15") != 15:
        errors.append(
            f"benchmark reported CPU15={metrics['CPU15']} instead of 15"
        )
    if control_value0 != iterations or control_value1 != iterations:
        errors.append(
            "control ping-pong values do not match iterations "
            f"({control_value0}, {control_value1}) vs {iterations}"
        )
    if value0 != iterations or value1 != iterations:
        errors.append(
            f"false-sharing values ({value0}, {value1}) do not match "
            f"iterations ({iterations})"
        )

    if control_avg <= 0.0:
        errors.append("control average latency is not positive")
    if false_avg <= 0.0:
        errors.append("false-sharing average latency is not positive")
    if false_avg <= control_avg:
        errors.append(
            "false-sharing average latency is not larger than the control "
            "average"
        )
    if delta_avg <= 0.0:
        errors.append("latency delta is not positive")

    for window in (control_window, false_window):
        if window.cpu0_l1d_accesses <= 0:
            errors.append(f"{window.name} window has no CPU0 L1D accesses")
        if window.cpu15_l1d_accesses <= 0:
            errors.append(f"{window.name} window has no CPU15 L1D accesses")

    if false_window.comp_ack_total <= control_window.comp_ack_total:
        errors.append(
            "false-sharing window does not increase CompAck traffic over "
            "the control window"
        )
    if false_window.send_comp_ack_total <= control_window.send_comp_ack_total:
        errors.append(
            "false-sharing window does not increase SendCompAck traffic over "
            "the control window"
        )
    if false_window.forward_sum <= control_window.forward_sum:
        errors.append(
            "false-sharing window does not increase forward diagonal flits "
            "over the control window"
        )
    if (
        false_window.forward_sum + false_window.reverse_sum
        <= control_window.forward_sum + control_window.reverse_sum
    ):
        errors.append(
            "false-sharing window does not increase total diagonal flits "
            "over the control window"
        )

    return AnalysisResult(
        m5out_dir=m5out_dir,
        console_path=console_path,
        command_line=metadata.get("command_line", ""),
        gem5_started=metadata.get("gem5_started", ""),
        iterations=iterations,
        shared_offset=metrics.get("SHARED_OFFSET", ""),
        cpu0=parse_int_metric(metrics, "CPU0"),
        cpu15=parse_int_metric(metrics, "CPU15"),
        control_avg=control_avg,
        control_min=parse_int_metric(metrics, "CONTROL_MIN"),
        control_max=parse_int_metric(metrics, "CONTROL_MAX"),
        false_avg=false_avg,
        false_min=parse_int_metric(metrics, "FALSE_MIN"),
        false_max=parse_int_metric(metrics, "FALSE_MAX"),
        delta_avg=delta_avg,
        control_value0=control_value0,
        control_value1=control_value1,
        value0=value0,
        value1=value1,
        stats_block_count=len(blocks),
        control_window=control_window,
        false_window=false_window,
        errors=errors,
    )


def format_path_lines(path: list[tuple[str, float]]) -> list[str]:
    return [f"  {name}: {value:.0f}" for name, value in path]


def format_window_report(window: WindowMetrics) -> list[str]:
    return [
        f"{window.name.capitalize()} window",
        f"  cpu0 accesses={window.cpu0_l1d_accesses:.0f} "
        f"misses={window.cpu0_l1d_misses:.0f}",
        f"  cpu15 accesses={window.cpu15_l1d_accesses:.0f} "
        f"misses={window.cpu15_l1d_misses:.0f}",
        f"  Store::total = {window.store_total:.0f}",
        f"  ReadUnique::total = {window.read_unique_total:.0f}",
        f"  CompAck::total = {window.comp_ack_total:.0f}",
        f"  SendCompAck::total = {window.send_comp_ack_total:.0f}",
        f"  forward flits = {window.forward_sum:.0f}",
        f"  reverse flits = {window.reverse_sum:.0f}",
    ]


def format_text_report(result: AnalysisResult) -> str:
    lines = [
        f"Iterations: {result.iterations}",
        f"Control avg: {result.control_avg:.2f}",
        f"False avg:   {result.false_avg:.2f}",
        f"Delta avg:   {result.delta_avg:.2f}",
        ("Control range: " f"{result.control_min}..{result.control_max}"),
        ("False range:   " f"{result.false_min}..{result.false_max}"),
        "",
        *format_window_report(result.control_window),
        "",
        *format_window_report(result.false_window),
        "",
        "False forward path flits",
        *format_path_lines(result.false_window.forward_path),
        "",
        "False reverse path flits",
        *format_path_lines(result.false_window.reverse_path),
    ]

    if result.errors:
        lines.extend(["", "FAIL"])
        lines.extend(f"  - {error}" for error in result.errors)
    else:
        lines.extend(
            [
                "",
                "PASS -- false-sharing ping-pong is slower than the control "
                "ping-pong and shows stronger coherence traffic",
            ]
        )

    return "\n".join(lines)


def format_markdown_report(result: AnalysisResult) -> str:
    lines = [
        "# False-Sharing Summary",
        "",
        "## Result",
        "",
        f"- Status: `{'PASS' if result.passed else 'FAIL'}`",
        f"- m5out directory: `{result.m5out_dir}`",
        f"- Console log: `{result.console_path}`",
        f"- Stats blocks observed: `{result.stats_block_count}` "
        "(checker uses the first two dumped windows)",
        f"- Iterations per participant: `{result.iterations}`",
        f"- Shared-line offset: `{result.shared_offset}`",
    ]

    if result.gem5_started:
        lines.append(f"- {result.gem5_started}")
    if result.command_line:
        lines.append(f"- Command line: `{result.command_line}`")

    lines.extend(
        [
            "",
            "## Benchmark Output",
            "",
            f"- CPU 0 participant: `{result.cpu0}`",
            f"- CPU 15 participant: `{result.cpu15}`",
            f"- Control ping-pong average: `{result.control_avg:.2f}` cycles",
            f"- False-sharing ping-pong average: "
            f"`{result.false_avg:.2f}` cycles",
            f"- Average latency delta: `{result.delta_avg:.2f}` cycles",
            f"- Control range: `{result.control_min}` .. "
            f"`{result.control_max}`",
            f"- False-sharing range: `{result.false_min}` .. "
            f"`{result.false_max}`",
            "",
            "## Control vs False-Sharing Latency",
            "",
            "| Phase | Average | Min | Max |",
            "| --- | ---: | ---: | ---: |",
            f"| `control` | {result.control_avg:.2f} | "
            f"{result.control_min} | {result.control_max} |",
            f"| `false-sharing` | {result.false_avg:.2f} | "
            f"{result.false_min} | {result.false_max} |",
            "",
            "## Control Window",
            "",
            "| Metric | Value |",
            "| --- | ---: |",
            f"| `cpu0 L1D accesses` | "
            f"{result.control_window.cpu0_l1d_accesses:.0f} |",
            f"| `cpu15 L1D accesses` | "
            f"{result.control_window.cpu15_l1d_accesses:.0f} |",
            f"| `Store::total` | {result.control_window.store_total:.0f} |",
            f"| `ReadUnique::total` | "
            f"{result.control_window.read_unique_total:.0f} |",
            f"| `CompAck::total` | "
            f"{result.control_window.comp_ack_total:.0f} |",
            f"| `SendCompAck::total` | "
            f"{result.control_window.send_comp_ack_total:.0f} |",
            f"| `forward diagonal flits` | "
            f"{result.control_window.forward_sum:.0f} |",
            f"| `reverse diagonal flits` | "
            f"{result.control_window.reverse_sum:.0f} |",
            "",
            "## False-Sharing Window",
            "",
            "| Metric | Value |",
            "| --- | ---: |",
            f"| `cpu0 L1D accesses` | "
            f"{result.false_window.cpu0_l1d_accesses:.0f} |",
            f"| `cpu15 L1D accesses` | "
            f"{result.false_window.cpu15_l1d_accesses:.0f} |",
            f"| `Store::total` | {result.false_window.store_total:.0f} |",
            f"| `ReadUnique::total` | "
            f"{result.false_window.read_unique_total:.0f} |",
            f"| `CompAck::total` | "
            f"{result.false_window.comp_ack_total:.0f} |",
            f"| `SendCompAck::total` | "
            f"{result.false_window.send_comp_ack_total:.0f} |",
            f"| `forward diagonal flits` | "
            f"{result.false_window.forward_sum:.0f} |",
            f"| `reverse diagonal flits` | "
            f"{result.false_window.reverse_sum:.0f} |",
            "",
            "## False-Sharing Path Detail",
            "",
            "| Link | Flits |",
            "| --- | ---: |",
        ]
    )

    for name, value in result.false_window.forward_path:
        lines.append(f"| `{name.split('.')[-1]}` | {value:.0f} |")

    lines.extend(
        [
            "",
            "## Interpretation",
            "",
            "- The control ping-pong measures the cost of the turn-taking "
            "protocol itself.",
            "- The false-sharing ping-pong adds one ownership transfer of the "
            "hot line in each direction.",
            "- The average latency delta therefore estimates the extra "
            "coherence cost of bouncing that line between CPU 0 and CPU 15.",
            "- The false-sharing stats window should show stronger CHI and "
            "overall diagonal-link activity than the control window.",
        ]
    )

    if result.errors:
        lines.extend(["", "## Failures", ""])
        lines.extend(f"- {error}" for error in result.errors)
    else:
        lines.extend(
            [
                "",
                "## Conclusion",
                "",
                "- Stage 3c passes for this run.",
                "- The ping-pong benchmark reports a positive latency delta "
                "between the control and false-sharing phases.",
                "- The false-sharing window also shows stronger coherence "
                "traffic than the control window, which matches the expected "
                "ownership-bounce behavior.",
            ]
        )

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
    print(format_text_report(result))
    if not result.passed:
        sys.exit(1)


if __name__ == "__main__":
    main()
