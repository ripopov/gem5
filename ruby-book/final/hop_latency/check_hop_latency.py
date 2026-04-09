#!/usr/bin/env python3
"""
check_hop_latency.py -- Analyse Stage 3b hop-latency output and stats.

Usage:
    python3 check_hop_latency.py <m5out-dir> <console-log>
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass
from typing import (
    Dict,
    Iterable,
    List,
    Tuple,
)

MIN_DELTA_CYCLES = 60.0

HNF0_EXT_NODE = "system.ruby.hnf00.cntrl"
HNF15_EXT_NODE = "system.ruby.hnf15.cntrl"

FAR_PATH_EDGES = [
    ("system.ruby.network.routers00", "system.ruby.network.routers01"),
    ("system.ruby.network.routers01", "system.ruby.network.routers02"),
    ("system.ruby.network.routers02", "system.ruby.network.routers03"),
    ("system.ruby.network.routers03", "system.ruby.network.routers07"),
    ("system.ruby.network.routers07", "system.ruby.network.routers11"),
    ("system.ruby.network.routers11", "system.ruby.network.routers15"),
]


@dataclass
class AnalysisResult:
    m5out_dir: str
    console_path: str
    command_line: str
    gem5_started: str
    sample_count: int
    near_offset: str
    far_offset: str
    near_samples: list[int]
    far_samples: list[int]
    near_avg: float
    far_avg: float
    delta_avg: float
    near_probe: int
    far_probe: int
    stats_block_count: int
    near_hnf0: float
    near_hnf15: float
    far_hnf0: float
    far_hnf15: float
    near_nonzero_hnfs: list[tuple[str, float]]
    far_nonzero_hnfs: list[tuple[str, float]]
    near_hnf0_ext: float
    near_hnf15_ext: float
    far_hnf0_ext: float
    far_hnf15_ext: float
    hnf0_ext_link: str
    hnf15_ext_link: str
    near_path: list[tuple[str, float]]
    far_path: list[tuple[str, float]]
    errors: list[str]

    @property
    def passed(self) -> bool:
        return not self.errors


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def parse_console(console_path: str) -> tuple[dict[str, str], dict[str, str]]:
    metrics: dict[str, str] = {}
    metadata: dict[str, str] = {"command_line": "", "gem5_started": ""}
    pattern = re.compile(r"^RBOOK_HOP\s+(\S+)\s+(.+?)\s*$")

    with open(console_path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            match = pattern.match(line)
            if match:
                metrics[match.group(1)] = match.group(2)
                continue

            if line.startswith("gem5 started "):
                metadata["gem5_started"] = line.strip()
            elif line.startswith("command line: "):
                metadata["command_line"] = line[
                    len("command line: ") :
                ].strip()

    for key in ("NEAR_AVG", "FAR_AVG", "DELTA_AVG"):
        if key not in metrics:
            fail(f"missing {key} in {console_path}")

    return metrics, metadata


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


def find_ext_link(sections: dict[str, dict[str, str]], ext_node: str) -> str:
    for section, values in sections.items():
        if not section.startswith("system.ruby.network.ext_links"):
            continue
        if values.get("ext_node") == ext_node:
            return section
    fail(f"could not find ext link for {ext_node}")


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


def ext_link_total(block: dict[str, float], section: str) -> float:
    return stat_value(
        block, f"{section}.network_links0.flits_per_vnet::total"
    ) + stat_value(block, f"{section}.network_links1.flits_per_vnet::total")


def int_link_total(block: dict[str, float], section: str) -> float:
    return stat_value(block, f"{section}.network_link.flits_per_vnet::total")


def nonzero_hnf_accesses(block: dict[str, float]) -> list[tuple[str, float]]:
    accesses = []

    for i in range(16):
        name = f"system.ruby.hnf{i}.cntrl.cache.m_demand_accesses"
        value = stat_value(block, name)
        if value != 0:
            accesses.append((f"hnf{i}", value))

    return accesses


def parse_int_metric(
    metrics: dict[str, str], key: str, default: int = 0
) -> int:
    value = metrics.get(key)
    if value is None:
        return default
    return int(value, 0)


def parse_sample_metric(metrics: dict[str, str], key: str) -> list[int]:
    value = metrics.get(key, "")
    if not value:
        return []
    return [int(token, 0) for token in value.split()]


def analyze_run(m5out_dir: str, console_path: str) -> AnalysisResult:
    stats_path = os.path.join(m5out_dir, "stats.txt")
    config_path = os.path.join(m5out_dir, "config.ini")

    for path in (console_path, stats_path, config_path):
        if not os.path.isfile(path):
            fail(f"missing required file: {path}")

    console_metrics, console_meta = parse_console(console_path)
    blocks = parse_stats_blocks(stats_path)
    sections = parse_config_sections(config_path)

    near_block = blocks[0]
    far_block = blocks[1]

    hnf0_ext = find_ext_link(sections, HNF0_EXT_NODE)
    hnf15_ext = find_ext_link(sections, HNF15_EXT_NODE)
    far_path_links = [
        find_int_link(sections, src, dst) for src, dst in FAR_PATH_EDGES
    ]

    near_avg = float(console_metrics["NEAR_AVG"])
    far_avg = float(console_metrics["FAR_AVG"])
    delta_avg = float(console_metrics["DELTA_AVG"])

    near_hnf0 = stat_value(
        near_block, "system.ruby.hnf0.cntrl.cache.m_demand_accesses"
    )
    near_hnf15 = stat_value(
        near_block, "system.ruby.hnf15.cntrl.cache.m_demand_accesses"
    )
    far_hnf0 = stat_value(
        far_block, "system.ruby.hnf0.cntrl.cache.m_demand_accesses"
    )
    far_hnf15 = stat_value(
        far_block, "system.ruby.hnf15.cntrl.cache.m_demand_accesses"
    )

    near_hnf0_ext = ext_link_total(near_block, hnf0_ext)
    near_hnf15_ext = ext_link_total(near_block, hnf15_ext)
    far_hnf0_ext = ext_link_total(far_block, hnf0_ext)
    far_hnf15_ext = ext_link_total(far_block, hnf15_ext)

    near_path = [
        (link, int_link_total(near_block, link)) for link in far_path_links
    ]
    far_path = [
        (link, int_link_total(far_block, link)) for link in far_path_links
    ]
    near_nonzero_hnfs = nonzero_hnf_accesses(near_block)
    far_nonzero_hnfs = nonzero_hnf_accesses(far_block)

    errors: list[str] = []

    if far_avg <= near_avg:
        errors.append(
            f"far average ({far_avg:.2f}) is not larger than near average ({near_avg:.2f})"
        )
    if delta_avg < MIN_DELTA_CYCLES:
        errors.append(
            f"delta average ({delta_avg:.2f}) is below the {MIN_DELTA_CYCLES:.0f}-cycle floor"
        )

    if near_hnf0 <= 0:
        errors.append("near isolated block has no HNF0 demand accesses")
    if near_hnf15 != 0:
        errors.append(
            f"near isolated block unexpectedly touched HNF15 ({near_hnf15:.0f} accesses)"
        )
    if far_hnf15 <= 0:
        errors.append("far isolated block has no HNF15 demand accesses")
    if far_hnf0 != 0:
        errors.append(
            f"far isolated block unexpectedly touched HNF0 ({far_hnf0:.0f} accesses)"
        )

    if near_hnf0_ext <= 0:
        errors.append("near isolated block has no HNF0 ext-link traffic")
    if near_hnf15_ext != 0:
        errors.append(
            f"near isolated block unexpectedly used HNF15 ext-link ({near_hnf15_ext:.0f} flits)"
        )
    if far_hnf15_ext <= 0:
        errors.append("far isolated block has no HNF15 ext-link traffic")
    if far_hnf0_ext != 0:
        errors.append(
            f"far isolated block unexpectedly used HNF0 ext-link ({far_hnf0_ext:.0f} flits)"
        )

    near_downstream_nonzero = [
        name for name, value in near_path[1:] if value != 0
    ]
    if near_downstream_nonzero:
        errors.append(
            "near isolated block unexpectedly reached deeper far-path links: "
            + ", ".join(near_downstream_nonzero)
        )
    if near_path[0][1] >= far_path[0][1]:
        errors.append(
            "near isolated block used the first eastbound path link as much as the far block"
        )

    far_path_zero = [name for name, value in far_path if value == 0]
    if far_path_zero:
        errors.append(
            "far isolated block missed expected XY path links: "
            + ", ".join(far_path_zero)
        )

    return AnalysisResult(
        m5out_dir=m5out_dir,
        console_path=console_path,
        command_line=console_meta.get("command_line", ""),
        gem5_started=console_meta.get("gem5_started", ""),
        sample_count=parse_int_metric(console_metrics, "SAMPLES"),
        near_offset=console_metrics.get("NEAR_OFFSET", ""),
        far_offset=console_metrics.get("FAR_OFFSET", ""),
        near_samples=parse_sample_metric(console_metrics, "NEAR_SAMPLES"),
        far_samples=parse_sample_metric(console_metrics, "FAR_SAMPLES"),
        near_avg=near_avg,
        far_avg=far_avg,
        delta_avg=delta_avg,
        near_probe=parse_int_metric(console_metrics, "NEAR_PROBE"),
        far_probe=parse_int_metric(console_metrics, "FAR_PROBE"),
        stats_block_count=len(blocks),
        near_hnf0=near_hnf0,
        near_hnf15=near_hnf15,
        far_hnf0=far_hnf0,
        far_hnf15=far_hnf15,
        near_nonzero_hnfs=near_nonzero_hnfs,
        far_nonzero_hnfs=far_nonzero_hnfs,
        near_hnf0_ext=near_hnf0_ext,
        near_hnf15_ext=near_hnf15_ext,
        far_hnf0_ext=far_hnf0_ext,
        far_hnf15_ext=far_hnf15_ext,
        hnf0_ext_link=hnf0_ext,
        hnf15_ext_link=hnf15_ext,
        near_path=near_path,
        far_path=far_path,
        errors=errors,
    )


def format_nonzero_hnfs(values: list[tuple[str, float]]) -> str:
    if not values:
        return "none"
    return ", ".join(f"{name}={value:.0f}" for name, value in values)


def format_path_lines(path_totals: Iterable[tuple[str, float]]) -> list[str]:
    return [f"  {name}: {value:.0f}" for name, value in path_totals]


def format_text_report(result: AnalysisResult) -> str:
    lines = [
        f"Near average cycles: {result.near_avg:.2f}",
        f"Far average cycles:  {result.far_avg:.2f}",
        f"Delta average:       {result.delta_avg:.2f}",
        "",
        "Isolated HNF demand accesses",
        f"  near block: hnf0={result.near_hnf0:.0f} hnf15={result.near_hnf15:.0f}",
        f"  far block:  hnf0={result.far_hnf0:.0f} hnf15={result.far_hnf15:.0f}",
        f"  near nonzero: {format_nonzero_hnfs(result.near_nonzero_hnfs)}",
        f"  far nonzero:  {format_nonzero_hnfs(result.far_nonzero_hnfs)}",
        "",
        "Isolated HNF ext-link totals",
        f"  near block: hnf0={result.near_hnf0_ext:.0f} hnf15={result.near_hnf15_ext:.0f}",
        f"  far block:  hnf0={result.far_hnf0_ext:.0f} hnf15={result.far_hnf15_ext:.0f}",
        "",
        "Near block far-path link totals",
        *format_path_lines(result.near_path),
        "",
        "Far block far-path link totals",
        *format_path_lines(result.far_path),
    ]

    if result.errors:
        lines.extend(["", "FAIL"])
        lines.extend(f"  - {error}" for error in result.errors)
    else:
        lines.extend(
            [
                "",
                "PASS -- far accesses are slower and the isolated stats match the expected path",
            ]
        )

    return "\n".join(lines)


def format_markdown_report(result: AnalysisResult) -> str:
    lines = [
        "# Hop-Distance Latency Summary",
        "",
        "## Result",
        "",
        f"- Status: `{'PASS' if result.passed else 'FAIL'}`",
        f"- m5out directory: `{result.m5out_dir}`",
        f"- Console log: `{result.console_path}`",
        f"- Stats blocks observed: `{result.stats_block_count}`",
    ]

    if result.gem5_started:
        lines.append(f"- {result.gem5_started}")
    if result.command_line:
        lines.append(f"- Command line: `{result.command_line}`")

    lines.extend(
        [
            "",
            "## Measurement Setup",
            "",
            f"- Samples per class: `{result.sample_count}`",
            f"- Near line offset: `{result.near_offset}`",
            f"- Far line offset: `{result.far_offset}`",
            "- Timed loads are bracketed by a 4 MiB private-cache eviction sweep.",
            "- Two isolated probes are wrapped with `m5_reset_stats()` and `m5_dump_reset_stats()`.",
            "",
            "## Latency",
            "",
            "| Metric | Cycles |",
            "| --- | ---: |",
            f"| Near average | {result.near_avg:.2f} |",
            f"| Far average | {result.far_avg:.2f} |",
            f"| Delta average | {result.delta_avg:.2f} |",
            f"| Near isolated probe | {result.near_probe:d} |",
            f"| Far isolated probe | {result.far_probe:d} |",
            "",
            "## Sample Vectors",
            "",
            f"- Near samples: `{' '.join(str(v) for v in result.near_samples)}`",
            f"- Far samples: `{' '.join(str(v) for v in result.far_samples)}`",
            "",
            "## Isolated HN-F Accesses",
            "",
            "| Block | Demand accesses | Nonzero slices |",
            "| --- | --- | --- |",
            f"| Near | `hnf0={result.near_hnf0:.0f}`, `hnf15={result.near_hnf15:.0f}` | {format_nonzero_hnfs(result.near_nonzero_hnfs)} |",
            f"| Far | `hnf0={result.far_hnf0:.0f}`, `hnf15={result.far_hnf15:.0f}` | {format_nonzero_hnfs(result.far_nonzero_hnfs)} |",
            "",
            "## Isolated Ext-Link Totals",
            "",
            "| Block | HNF0 ext-link | HNF15 ext-link |",
            "| --- | ---: | ---: |",
            f"| Near | {result.near_hnf0_ext:.0f} | {result.near_hnf15_ext:.0f} |",
            f"| Far | {result.far_hnf0_ext:.0f} | {result.far_hnf15_ext:.0f} |",
            "",
            "## Expected Far XY Path",
            "",
            "| Link | Near block flits | Far block flits |",
            "| --- | ---: | ---: |",
        ]
    )

    for (near_name, near_value), (_, far_value) in zip(
        result.near_path, result.far_path
    ):
        short_name = near_name.split(".")[-1]
        lines.append(
            f"| `{short_name}` | {near_value:.0f} | {far_value:.0f} |"
        )

    lines.extend(
        [
            "",
            "## Interpretation",
            "",
            f"- The far load is slower by `{result.delta_avg:.2f}` cycles.",
            "- The isolated near probe stays local to `hnf0` and does not traverse the full diagonal path.",
            "- The isolated far probe reaches `hnf15` and activates all expected XY mesh links.",
            "- Small extra slice accesses can still appear inside the isolated windows because the benchmark itself executes instructions and accesses stack data while stats are enabled.",
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
                "- Stage 3b passes for this run.",
                "- The observed latency delta matches the expected hop-distance penalty and the isolated network evidence matches the intended near-versus-far interpretation.",
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
