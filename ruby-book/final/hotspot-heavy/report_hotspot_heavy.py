#!/usr/bin/env python3
"""
report_hotspot_heavy.py -- Generate a Markdown summary report.

Usage:
    python3 report_hotspot_heavy.py <m5out-dir> <console-log> [output.md]
"""

from __future__ import annotations

import sys

from check_hotspot_heavy import (
    analyze_run,
    format_markdown_report,
)


def main() -> None:
    if len(sys.argv) not in (3, 4):
        print(
            f"Usage: {sys.argv[0]} <m5out-dir> <console-log> [output.md]",
            file=sys.stderr,
        )
        sys.exit(1)

    result = analyze_run(sys.argv[1], sys.argv[2])
    report = format_markdown_report(result)

    if len(sys.argv) == 4:
        output_path = sys.argv[3]
        with open(output_path, "w", encoding="utf-8") as fh:
            fh.write(report)
        print(output_path)
    else:
        print(report, end="")

    if not result.passed:
        sys.exit(1)


if __name__ == "__main__":
    main()
