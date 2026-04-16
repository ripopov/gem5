#!/usr/bin/env python3
"""
report_link_pressure.py -- Write a Markdown summary of a link-pressure run.

Usage:
    python3 report_link_pressure.py <m5out-dir> <console-log> <output-md>
"""

from __future__ import annotations

import sys

from check_link_pressure import analyze_run


def format_markdown(result) -> str:
    lines = [
        "# Link Pressure Summary",
        "",
        "## Result",
        "",
        f"- Status: `{'PASS' if result.passed else 'FAIL'}`",
        f"- m5out directory: `{result.m5out_dir}`",
        f"- Console log: `{result.console_path}`",
        f"- Stream pages: `{result.stream_pages}` "
        f"(buffer `{result.buffer_bytes // (1024 * 1024)} MiB`, "
        f"`{result.total_lines}` cache lines)",
        f"- Stats blocks observed: `{result.stats_block_count}` "
        f"(report uses the first 5 dumped windows)",
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
            "| Threads | Ops | Cycles | Line ops/cycle | Avg flit queueing | Avg flit network | Flits received | HNF max | HNF min | HNF spread (max/min) |",
            "| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
        ]
    )
    for w in result.windows:
        lines.append(
            f"| {w.console.threads} | {w.console.ops} | {w.console.cycles} "
            f"| {w.console.throughput:.6f} | {w.queueing_latency:.2f} "
            f"| {w.network_latency:.2f} | {w.flits_received_total:.0f} "
            f"| {w.hnf_max:.0f} | {w.hnf_min:.0f} | {w.hnf_spread:.3f} |"
        )

    lines.extend(["", "## Per-HNF Demand Accesses (16-thread window)", ""])
    high = result.windows[-1]
    lines.append("| HNF | Demand accesses |")
    lines.append("| ---: | ---: |")
    for i, value in enumerate(high.hnf_accesses):
        lines.append(f"| {i} | {value:.0f} |")

    if result.errors:
        lines.extend(["", "## Failures", ""])
        for e in result.errors:
            lines.append(f"- {e}")

    lines.append("")
    return "\n".join(lines)


def main() -> None:
    if len(sys.argv) != 4:
        print(
            f"Usage: {sys.argv[0]} <m5out-dir> <console-log> <output-md>",
            file=sys.stderr,
        )
        sys.exit(1)
    result = analyze_run(sys.argv[1], sys.argv[2])
    with open(sys.argv[3], "w", encoding="utf-8") as fh:
        fh.write(format_markdown(result))


if __name__ == "__main__":
    main()
