#!/usr/bin/env python3
"""
report_smoke.py -- Generate a Markdown summary report for Stage 3a.

Usage:
    python3 report_smoke.py <m5out-dir> [console-log] [output.md]
"""

from __future__ import annotations

import sys

from check_smoke import (
    analyze_run,
    format_markdown_report,
)


def main() -> None:
    if len(sys.argv) not in (2, 3, 4):
        print(
            f"Usage: {sys.argv[0]} <m5out-dir> [console-log] [output.md]",
            file=sys.stderr,
        )
        sys.exit(1)

    console_path = sys.argv[2] if len(sys.argv) >= 3 else None
    output_path = sys.argv[3] if len(sys.argv) == 4 else None

    result = analyze_run(sys.argv[1], console_path)
    report = format_markdown_report(result)

    if output_path is None:
        print(report, end="")
    else:
        with open(output_path, "w", encoding="utf-8") as fh:
            fh.write(report)
        print(output_path)

    if not result.passed:
        sys.exit(1)


if __name__ == "__main__":
    main()
