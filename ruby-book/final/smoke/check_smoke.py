#!/usr/bin/env python3
"""
check_smoke.py -- Analyse smoke-test stats and verify all 16 HN-F slices.

Usage:
    python3 check_smoke.py <m5out-dir> [console-log]
"""

from __future__ import annotations

import os
import re
import sys
from dataclasses import dataclass

NUM_HNFS = 16
# Accounts for instruction fetches, stack accesses, and alignment effects.
MAX_IMBALANCE = 0.20


@dataclass
class AnalysisResult:
    m5out_dir: str
    console_path: str
    command_line: str
    gem5_started: str
    saw_pass: bool
    accesses: dict[int, int]
    total: int
    mean: float
    errors: list[str]

    @property
    def passed(self) -> bool:
        return not self.errors


def fail(message: str) -> None:
    print(f"FAIL: {message}", file=sys.stderr)
    sys.exit(1)


def parse_console(console_path: str) -> tuple[dict[str, str], bool]:
    metadata = {"command_line": "", "gem5_started": ""}
    saw_pass = False

    with open(console_path, encoding="utf-8") as fh:
        for raw in fh:
            line = raw.strip()
            if line == "PASS":
                saw_pass = True
            elif line.startswith("gem5 started "):
                metadata["gem5_started"] = line
            elif line.startswith("command line: "):
                metadata["command_line"] = line[len("command line: ") :]

    return metadata, saw_pass


def parse_accesses(stats_path: str) -> dict[int, int]:
    pattern = re.compile(
        r"system\.ruby\.hnf(\d+)\.cntrl\.cache\.m_demand_accesses\s+(\d+)"
    )
    accesses: dict[int, int] = {}

    with open(stats_path, encoding="utf-8") as fh:
        for line in fh:
            match = pattern.search(line)
            if match:
                accesses[int(match.group(1))] = int(match.group(2))

    return accesses


def analyze_run(
    m5out_dir: str, console_path: str | None = None
) -> AnalysisResult:
    stats_path = os.path.join(m5out_dir, "stats.txt")
    if not os.path.isfile(stats_path):
        fail(f"missing required file: {stats_path}")

    if console_path is None:
        console_path = os.path.join(m5out_dir, "console.log")
    if not os.path.isfile(console_path):
        fail(f"missing required file: {console_path}")

    metadata, saw_pass = parse_console(console_path)
    accesses = parse_accesses(stats_path)
    total = sum(accesses.values())
    mean = total / len(accesses) if accesses else 0.0

    errors = []
    if not saw_pass:
        errors.append("benchmark did not print PASS")

    missing = set(range(NUM_HNFS)) - set(accesses)
    if missing:
        errors.append(f"missing HN-F slices: {sorted(missing)}")

    zeros = [i for i in sorted(accesses) if accesses[i] == 0]
    if zeros:
        errors.append(f"HN-F slices with zero accesses: {zeros}")

    if mean > 0:
        for i in sorted(accesses):
            deviation = abs(accesses[i] - mean) / mean
            if deviation > MAX_IMBALANCE:
                errors.append(
                    f"HN-F {i}: {accesses[i]} accesses, "
                    f"{deviation:.1%} from mean ({mean:.0f}) exceeds "
                    f"{MAX_IMBALANCE:.0%} threshold"
                )

    return AnalysisResult(
        m5out_dir=m5out_dir,
        console_path=console_path,
        command_line=metadata["command_line"],
        gem5_started=metadata["gem5_started"],
        saw_pass=saw_pass,
        accesses=accesses,
        total=total,
        mean=mean,
        errors=errors,
    )


def _format_table_rows(result: AnalysisResult) -> list[str]:
    rows = [f"{'HN-F':>6}  {'Accesses':>10}", f"{'----':>6}  {'--------':>10}"]
    for i in range(NUM_HNFS):
        value = result.accesses.get(i, 0)
        rows.append(f"{i:>6}  {value:>10}")
    return rows


def format_text_report(result: AnalysisResult) -> str:
    lines = []
    if result.gem5_started:
        lines.append(result.gem5_started)
    if result.command_line:
        lines.append(f"command line: {result.command_line}")
    lines.append(f"m5out dir: {result.m5out_dir}")
    lines.append(f"console log: {result.console_path}")
    lines.append("")
    lines.extend(_format_table_rows(result))
    lines.append("")
    lines.append(f"{'Total':>6}  {result.total:>10}")
    lines.append(f"{'Mean':>6}  {result.mean:>10.1f}")

    if result.passed:
        lines.append("")
        lines.append("PASS -- all 16 HN-F slices are nonzero and balanced")
    else:
        lines.append("")
        lines.append("FAIL")
        for error in result.errors:
            lines.append(f"  - {error}")

    return "\n".join(lines) + "\n"


def format_markdown_report(result: AnalysisResult) -> str:
    lines = ["# Stage 3a Smoke Test Report", ""]
    lines.append(f"- Result: {'PASS' if result.passed else 'FAIL'}")
    lines.append(f"- Run directory: `{result.m5out_dir}`")
    lines.append(f"- Console log: `{result.console_path}`")
    if result.gem5_started:
        lines.append(f"- gem5 started: `{result.gem5_started}`")
    if result.command_line:
        lines.append(f"- Command line: `{result.command_line}`")
    lines.append(f"- Total HN-F demand accesses: `{result.total}`")
    lines.append(f"- Mean per HN-F: `{result.mean:.1f}`")
    lines.append("")
    lines.append("## Per-HN-F Accesses")
    lines.append("")
    lines.append("| HN-F | Accesses |")
    lines.append("| ---: | -------: |")
    for i in range(NUM_HNFS):
        lines.append(f"| {i} | {result.accesses.get(i, 0)} |")

    lines.append("")
    if result.passed:
        lines.append("## Checks")
        lines.append("")
        lines.append(
            "All 16 HN-F slices were observed, all were nonzero, and all "
            f"stayed within the {MAX_IMBALANCE:.0%} imbalance threshold."
        )
    else:
        lines.append("## Failures")
        lines.append("")
        for error in result.errors:
            lines.append(f"- {error}")

    lines.append("")
    return "\n".join(lines)


def main() -> None:
    if len(sys.argv) not in (2, 3):
        print(
            f"Usage: {sys.argv[0]} <m5out-dir> [console-log]",
            file=sys.stderr,
        )
        sys.exit(1)

    console_path = sys.argv[2] if len(sys.argv) == 3 else None
    result = analyze_run(sys.argv[1], console_path)
    print(format_text_report(result), end="")
    if not result.passed:
        sys.exit(1)


if __name__ == "__main__":
    main()
