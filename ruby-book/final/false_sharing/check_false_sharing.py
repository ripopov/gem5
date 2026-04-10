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
class AnalysisResult:
    m5out_dir: str
    console_path: str
    command_line: str
    gem5_started: str
    iterations: int
    shared_offset: str
    cpu0: int
    cpu15: int
    cpu0_cycles: int
    cpu15_cycles: int
    value0: int
    value1: int
    stats_block_count: int
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

    for key in ("ITERATIONS", "CPU0", "CPU15", "VALUE0", "VALUE1"):
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

    if not blocks:
        fail(f"expected at least 1 dumped stats block in {stats_path}")

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


def analyze_run(m5out_dir: str, console_path: str) -> AnalysisResult:
    stats_path = os.path.join(m5out_dir, "stats.txt")
    config_path = os.path.join(m5out_dir, "config.ini")

    for path in (console_path, stats_path, config_path):
        if not os.path.isfile(path):
            fail(f"missing required file: {path}")

    metrics, metadata, saw_pass = parse_console(console_path)
    blocks = parse_stats_blocks(stats_path)
    sections = parse_config_sections(config_path)
    measured_block = blocks[0]

    forward_links = [
        find_int_link(sections, src, dst) for src, dst in FORWARD_PATH_EDGES
    ]
    reverse_links = [
        find_int_link(sections, src, dst) for src, dst in REVERSE_PATH_EDGES
    ]
    forward_path = [
        (link, int_link_total(measured_block, link)) for link in forward_links
    ]
    reverse_path = [
        (link, int_link_total(measured_block, link)) for link in reverse_links
    ]

    iterations = parse_int_metric(metrics, "ITERATIONS")
    value0 = parse_int_metric(metrics, "VALUE0")
    value1 = parse_int_metric(metrics, "VALUE1")

    cpu0_l1d_accesses = stat_value(
        measured_block, "system.cpu0.l1d.cache.m_demand_accesses"
    )
    cpu0_l1d_misses = stat_value(
        measured_block, "system.cpu0.l1d.cache.m_demand_misses"
    )
    cpu15_l1d_accesses = stat_value(
        measured_block, "system.cpu15.l1d.cache.m_demand_accesses"
    )
    cpu15_l1d_misses = stat_value(
        measured_block, "system.cpu15.l1d.cache.m_demand_misses"
    )

    read_unique_total = stat_value(
        measured_block, "system.ruby.Cache_Controller.ReadUnique::total"
    )
    store_total = stat_value(
        measured_block, "system.ruby.Cache_Controller.Store::total"
    )
    comp_ack_total = stat_value(
        measured_block, "system.ruby.Cache_Controller.CompAck::total"
    )
    send_comp_ack_total = stat_value(
        measured_block, "system.ruby.Cache_Controller.SendCompAck::total"
    )

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
    if value0 != iterations or value1 != iterations:
        errors.append(
            f"final values ({value0}, {value1}) do not match iterations ({iterations})"
        )

    if cpu0_l1d_accesses <= 0:
        errors.append("measured block has no CPU0 L1D demand accesses")
    if cpu15_l1d_accesses <= 0:
        errors.append("measured block has no CPU15 L1D demand accesses")
    if cpu0_l1d_misses <= 0:
        errors.append("measured block has no CPU0 L1D demand misses")
    if cpu15_l1d_misses <= 0:
        errors.append("measured block has no CPU15 L1D demand misses")

    if read_unique_total <= 0:
        errors.append("measured block has no ReadUnique traffic")
    if comp_ack_total <= 0:
        errors.append("measured block has no CompAck traffic")
    if send_comp_ack_total <= 0:
        errors.append("measured block has no SendCompAck traffic")
    if store_total < 2 * iterations:
        errors.append(
            f"store total ({store_total:.0f}) is smaller than 2 * iterations ({2 * iterations})"
        )

    missing_forward = [name for name, value in forward_path if value <= 0]
    if missing_forward:
        errors.append(
            "measured block missed forward diagonal links: "
            + ", ".join(missing_forward)
        )

    missing_reverse = [name for name, value in reverse_path if value <= 0]
    if missing_reverse:
        errors.append(
            "measured block missed reverse diagonal links: "
            + ", ".join(missing_reverse)
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
        cpu0_cycles=parse_int_metric(metrics, "CPU0_CYCLES"),
        cpu15_cycles=parse_int_metric(metrics, "CPU15_CYCLES"),
        value0=value0,
        value1=value1,
        stats_block_count=len(blocks),
        cpu0_l1d_accesses=cpu0_l1d_accesses,
        cpu0_l1d_misses=cpu0_l1d_misses,
        cpu15_l1d_accesses=cpu15_l1d_accesses,
        cpu15_l1d_misses=cpu15_l1d_misses,
        read_unique_total=read_unique_total,
        store_total=store_total,
        comp_ack_total=comp_ack_total,
        send_comp_ack_total=send_comp_ack_total,
        forward_path=forward_path,
        reverse_path=reverse_path,
        errors=errors,
    )


def format_path_lines(path: list[tuple[str, float]]) -> list[str]:
    return [f"  {name}: {value:.0f}" for name, value in path]


def format_text_report(result: AnalysisResult) -> str:
    lines = [
        f"Iterations: {result.iterations}",
        f"CPU0 cycles:  {result.cpu0_cycles}",
        f"CPU15 cycles: {result.cpu15_cycles}",
        f"Final values: value0={result.value0} value1={result.value1}",
        "",
        "CPU L1D demand activity",
        f"  cpu0 accesses={result.cpu0_l1d_accesses:.0f} misses={result.cpu0_l1d_misses:.0f}",
        f"  cpu15 accesses={result.cpu15_l1d_accesses:.0f} misses={result.cpu15_l1d_misses:.0f}",
        "",
        "CHI counters",
        f"  Store::total = {result.store_total:.0f}",
        f"  ReadUnique::total = {result.read_unique_total:.0f}",
        f"  CompAck::total = {result.comp_ack_total:.0f}",
        f"  SendCompAck::total = {result.send_comp_ack_total:.0f}",
        "",
        "Forward path flits",
        *format_path_lines(result.forward_path),
        "",
        "Reverse path flits",
        *format_path_lines(result.reverse_path),
    ]

    if result.errors:
        lines.extend(["", "FAIL"])
        lines.extend(f"  - {error}" for error in result.errors)
    else:
        lines.extend(
            [
                "",
                "PASS -- false-sharing traffic exercised both CPUs and both diagonal path directions",
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
        f"- Stats blocks observed: `{result.stats_block_count}`",
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
            f"- CPU 0 loop cycles: `{result.cpu0_cycles}`",
            f"- CPU 15 loop cycles: `{result.cpu15_cycles}`",
            f"- Final value0: `{result.value0}`",
            f"- Final value1: `{result.value1}`",
            "",
            "## L1D Demand Activity",
            "",
            "| CPU | Accesses | Misses |",
            "| --- | ---: | ---: |",
            f"| `cpu0` | {result.cpu0_l1d_accesses:.0f} | {result.cpu0_l1d_misses:.0f} |",
            f"| `cpu15` | {result.cpu15_l1d_accesses:.0f} | {result.cpu15_l1d_misses:.0f} |",
            "",
            "## CHI Counters",
            "",
            "| Counter | Total |",
            "| --- | ---: |",
            f"| `Store::total` | {result.store_total:.0f} |",
            f"| `ReadUnique::total` | {result.read_unique_total:.0f} |",
            f"| `CompAck::total` | {result.comp_ack_total:.0f} |",
            f"| `SendCompAck::total` | {result.send_comp_ack_total:.0f} |",
            "",
            "## Forward Diagonal Path",
            "",
            "| Link | Flits |",
            "| --- | ---: |",
        ]
    )

    for name, value in result.forward_path:
        lines.append(f"| `{name.split('.')[-1]}` | {value:.0f} |")

    lines.extend(
        [
            "",
            "## Reverse Diagonal Path",
            "",
            "| Link | Flits |",
            "| --- | ---: |",
        ]
    )

    for name, value in result.reverse_path:
        lines.append(f"| `{name.split('.')[-1]}` | {value:.0f} |")

    lines.extend(
        [
            "",
            "## Interpretation",
            "",
            "- Both participants touched the same cache line from opposite corners of the mesh.",
            "- Nonzero `ReadUnique` and `CompAck` counters indicate coherence-mediated ownership transfers.",
            "- Nonzero flits on both diagonal directions show that requests and invalidation/response traffic crossed the mesh rather than staying local.",
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
                "- The measured window shows the expected two-core false-sharing pattern: both L1s participate, CHI ownership changes occur, and mesh traffic appears in both diagonal directions.",
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
