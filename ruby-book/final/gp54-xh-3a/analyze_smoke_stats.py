#!/usr/bin/env python3

import argparse
import configparser
import math
import re
from pathlib import Path

STAT_LINE = re.compile(
    r"^(?P<name>\S+)\s+(?P<value>[-+]?\d+(?:\.\d+)?(?:[eE][-+]?\d+)?)\b"
)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Analyze stage 3a smoke-test Ruby stats"
    )
    parser.add_argument(
        "--run-dir",
        required=True,
        help="gem5 output directory containing stats.txt/config.ini/simout",
    )
    parser.add_argument(
        "--out",
        required=True,
        help="Markdown report path to write",
    )
    return parser.parse_args()


def normalize_hnf_controller(section: str):
    match = re.fullmatch(r"(system\.ruby\.hnf)0*(\d+)(\.cntrl)", section)
    if not match:
        return None
    return f"{match.group(1)}{int(match.group(2))}{match.group(3)}"


def load_l3_controllers(config_ini: Path):
    parser = configparser.ConfigParser(strict=False)
    parser.optionxform = str
    parser.read(config_ini)

    controllers = []
    for section in parser.sections():
        if not section.startswith("system.ruby.hnf"):
            continue
        if not section.endswith(".cntrl"):
            continue
        if parser.get(section, "type", fallback="") != "CHI_Cache_Controller":
            continue
        if parser.get(section, "is_HN", fallback="false") != "true":
            continue

        normalized = normalize_hnf_controller(section)
        if normalized is not None:
            controllers.append(normalized)
    return sorted(controllers)


def parse_stats_blocks(stats_path: Path):
    blocks = []
    current = None

    with stats_path.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if "Begin Simulation Statistics" in line:
                current = {}
                continue
            if "End Simulation Statistics" in line:
                if current is not None:
                    blocks.append(current)
                current = None
                continue
            if current is None or not line or line.startswith("#"):
                continue

            match = STAT_LINE.match(line)
            if not match:
                continue

            current[match.group("name")] = float(match.group("value"))

    return blocks


def block_slice_stats(block, controllers):
    entries = []
    total_accesses = 0.0
    for controller in controllers:
        hits = block.get(f"{controller}.cache.m_demand_hits", 0.0)
        misses = block.get(f"{controller}.cache.m_demand_misses", 0.0)
        total = hits + misses
        entries.append(
            {
                "controller": controller,
                "hits": hits,
                "misses": misses,
                "total": total,
            }
        )
        total_accesses += total
    return entries, total_accesses


def select_measurement_block(blocks, controllers):
    candidates = []
    for index, block in enumerate(blocks, start=1):
        entries, total_accesses = block_slice_stats(block, controllers)
        candidates.append((index, block, entries, total_accesses))

    for candidate in candidates:
        if candidate[3] > 0:
            return candidate

    if not candidates:
        raise RuntimeError("stats.txt does not contain any statistics blocks")

    return candidates[-1]


def read_optional_text(path: Path):
    if not path.exists():
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def format_int(value):
    if math.isclose(value, round(value)):
        return f"{int(round(value)):,}"
    return f"{value:,.2f}"


def main():
    args = parse_args()
    run_dir = Path(args.run_dir).resolve()
    out_path = Path(args.out).resolve()

    config_ini = run_dir / "config.ini"
    stats_txt = run_dir / "stats.txt"
    simout = run_dir / "simout"
    console_log = run_dir / "console.log"

    controllers = load_l3_controllers(config_ini)
    if len(controllers) != 16:
        raise RuntimeError(
            f"expected 16 HN-F controllers, found {len(controllers)}"
        )

    blocks = parse_stats_blocks(stats_txt)
    block_index, _, entries, total_accesses = select_measurement_block(
        blocks, controllers
    )

    totals = [entry["total"] for entry in entries]
    nonzero = sum(1 for value in totals if value > 0)
    mean_total = total_accesses / len(entries)
    min_total = min(totals)
    max_total = max(totals)
    spread_pct = (
        0.0
        if mean_total == 0
        else ((max_total - min_total) / mean_total) * 100.0
    )

    simout_text = read_optional_text(simout) + read_optional_text(console_log)
    saw_pass = "PASS" in simout_text
    exit_line = ""
    pass_line = ""
    for line in simout_text.splitlines():
        if line.startswith("PASS"):
            pass_line = line
        if line.startswith("Exiting @ tick"):
            exit_line = line

    lines = [
        "# Stage 3a Smoke Test Report",
        "",
        f"Run directory: `{run_dir}`",
        f"Measurement block: {block_index} of {len(blocks)}",
        f"PASS observed in run log: {'yes' if saw_pass else 'no'}",
    ]

    if exit_line:
        lines.append(f"Exit status: `{exit_line}`")
    if pass_line:
        lines.append(f"Workload output: `{pass_line}`")

    lines.extend(
        [
            "",
            "## Summary",
            "",
            f"- L3 slices with nonzero demand accesses: {nonzero}/16",
            f"- Total L3 demand accesses: {format_int(total_accesses)}",
            f"- Per-slice average: {format_int(mean_total)} accesses",
            f"- Per-slice min/max: {format_int(min_total)} / {format_int(max_total)}",
            f"- Max-min spread: {spread_pct:.2f}% of the mean",
        ]
    )

    if nonzero == 16:
        lines.append(
            "- Result: all 16 HN-F slices were reached by the sequential sweep."
        )
    else:
        lines.append(
            "- Result: one or more HN-F slices saw zero accesses, which indicates"
            " a mapping or topology problem."
        )

    if spread_pct <= 5.0:
        lines.append(
            "- Balance check: the slice totals are close enough to call the"
            " distribution roughly balanced for this smoke test."
        )
    else:
        lines.append(
            "- Balance check: the per-slice totals are visibly skewed and deserve"
            " follow-up inspection."
        )

    lines.extend(
        [
            "",
            "## Per-Slice Demand Accesses",
            "",
            "| Slice | Hits | Misses | Total |",
            "| --- | ---: | ---: | ---: |",
        ]
    )

    for index, entry in enumerate(entries):
        lines.append(
            "| {slice_idx} | {hits} | {misses} | {total} |".format(
                slice_idx=index,
                hits=format_int(entry["hits"]),
                misses=format_int(entry["misses"]),
                total=format_int(entry["total"]),
            )
        )

    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
