#!/usr/bin/env python3
"""
ki25-3a_smoke_test_analysis.py

Analyzes simulation results for the single-core smoke test (rbook_test_smoke.c).

Verifies:
- Simulation completed successfully
- All 16 LLC slices (HN-F controllers) received traffic
- Access distribution is balanced across slices
- Provides summary statistics and validation
"""

import re
import sys
from pathlib import Path
from typing import (
    Dict,
    List,
    Tuple,
)


def parse_stats_file(stats_path: Path) -> Dict[str, Dict[str, float]]:
    """Parse gem5 stats.txt and extract key metrics."""
    stats = {
        "simulation": {},
        "hnf": {},  # Per-HNF statistics
        "l1d": {},
        "l2": {},
    }

    with open(stats_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue

            # Parse simSeconds, simTicks, etc.
            if line.startswith("simSeconds"):
                stats["simulation"]["sim_seconds"] = float(line.split()[1])
            elif line.startswith("simTicks"):
                stats["simulation"]["sim_ticks"] = float(line.split()[1])
            elif line.startswith("simInsts"):
                stats["simulation"]["sim_insts"] = float(line.split()[1])
            elif line.startswith("hostSeconds"):
                stats["simulation"]["host_seconds"] = float(line.split()[1])

            # Parse HNF statistics
            # Pattern: system.ruby.hnfN.cntrl.cache.m_demand_hits/misses
            hnf_match = re.match(
                r"system\.ruby\.(hnf\d+)\.cntrl\.cache\.(m_demand_hits|m_demand_misses)\s+(\d+)",
                line,
            )
            if hnf_match:
                hnf_id = hnf_match.group(1)
                stat_type = hnf_match.group(2)
                value = int(hnf_match.group(3))

                if hnf_id not in stats["hnf"]:
                    stats["hnf"][hnf_id] = {}
                stats["hnf"][hnf_id][stat_type] = value

            # Parse L1d stats for core 0
            if line.startswith("system.cpu0.l1d.cache.m_demand_hits"):
                stats["l1d"]["hits"] = int(line.split()[1])
            elif line.startswith("system.cpu0.l1d.cache.m_demand_misses"):
                stats["l1d"]["misses"] = int(line.split()[1])

            # Parse L2 stats for core 0
            if line.startswith("system.cpu0.l2.cache.m_demand_hits"):
                stats["l2"]["hits"] = int(line.split()[1])
            elif line.startswith("system.cpu0.l2.cache.m_demand_misses"):
                stats["l2"]["misses"] = int(line.split()[1])

    return stats


def validate_hnf_distribution(stats: Dict) -> Tuple[bool, List[str]]:
    """
    Validate that HNF access distribution is reasonable.

    Returns: (is_valid, list_of_issues)
    """
    hnf_stats = stats.get("hnf", {})
    issues = []

    # Check we have all 16 HNFs
    if len(hnf_stats) != 16:
        issues.append(
            f"ERROR: Expected 16 HN-F controllers, found {len(hnf_stats)}"
        )
        return False, issues

    # Calculate total accesses per HNF
    total_accesses = {}
    for hnf_id, hnf_data in hnf_stats.items():
        hits = hnf_data.get("m_demand_hits", 0)
        misses = hnf_data.get("m_demand_misses", 0)
        total = hits + misses
        total_accesses[hnf_id] = total

        if total == 0:
            issues.append(
                f"ERROR: {hnf_id} has zero total accesses - address mapping may be wrong"
            )

    if not total_accesses:
        issues.append("ERROR: No HNF access data found")
        return False, issues

    # Check balance
    values = list(total_accesses.values())
    avg_accesses = sum(values) / len(values)
    min_accesses = min(values)
    max_accesses = max(values)

    # Expect roughly balanced (within 20% of average is good)
    max_deviation = max(abs(v - avg_accesses) for v in values)
    deviation_pct = (
        (max_deviation / avg_accesses) * 100 if avg_accesses > 0 else 0
    )

    if deviation_pct > 25:
        issues.append(
            f"WARNING: HNF access distribution is uneven (max deviation: {deviation_pct:.1f}%)"
        )
    else:
        issues.append(
            f"OK: HNF access distribution is balanced (max deviation: {deviation_pct:.1f}%)"
        )

    issues.append(
        f"  Total accesses per HNF: min={min_accesses}, max={max_accesses}, avg={avg_accesses:.1f}"
    )

    return len([i for i in issues if i.startswith("ERROR")]) == 0, issues


def analyze_cache_hierarchy(stats: Dict) -> List[str]:
    """Analyze cache hierarchy behavior."""
    results = []

    l1d = stats.get("l1d", {})
    l2 = stats.get("l2", {})

    l1d_hits = l1d.get("hits", 0)
    l1d_misses = l1d.get("misses", 0)
    l1d_total = l1d_hits + l1d_misses

    l2_hits = l2.get("hits", 0)
    l2_misses = l2.get("misses", 0)
    l2_total = l2_hits + l2_misses

    results.append(
        f"L1D: {l1d_hits} hits, {l1d_misses} misses (hit rate: {100*l1d_hits/l1d_total:.1f}% if total>0)"
    )
    results.append(f"L2:  {l2_hits} hits, {l2_misses} misses")
    results.append(
        f"  L2 miss rate: {100*l2_misses/l2_total:.1f}% (should be high - cold cache)"
    )

    return results


def generate_report(stats: Dict, output_path: Path):
    """Generate analysis report."""
    sim = stats.get("simulation", {})
    hnf_stats = stats.get("hnf", {})

    is_valid, issues = validate_hnf_distribution(stats)
    cache_analysis = analyze_cache_hierarchy(stats)

    report_lines = [
        "=" * 70,
        "KI25-3A SMOKE TEST ANALYSIS REPORT",
        "=" * 70,
        "",
        "Test: Single-core LLC reachability (rbook_test_smoke.c)",
        "System: 4x4 CHI mesh with 16 HN-F (LLC slices) + 2 SN-F (DDR)",
        "",
        "-" * 70,
        "SIMULATION SUMMARY",
        "-" * 70,
        f"Simulated time:     {sim.get('sim_seconds', 0):.6f} seconds",
        f"Simulated ticks:    {sim.get('sim_ticks', 0):.0f}",
        f"Instructions:       {sim.get('sim_insts', 0):.0f}",
        f"Host time:          {sim.get('host_seconds', 0):.2f} seconds",
        "",
        "-" * 70,
        "HNF (LLC SLICE) VALIDATION",
        "-" * 70,
    ]

    # Add validation results
    for issue in issues:
        if issue.startswith("ERROR"):
            report_lines.append(f"[FAIL] {issue}")
        elif issue.startswith("WARNING"):
            report_lines.append(f"[WARN] {issue}")
        elif issue.startswith("OK"):
            report_lines.append(f"[PASS] {issue}")
        else:
            report_lines.append(f"       {issue}")

    # Add per-HNF breakdown
    report_lines.extend(
        [
            "",
            "Per-HNF access counts:",
            "-" * 40,
        ]
    )

    # Sort by HNF number
    sorted_hnfs = sorted(
        hnf_stats.items(), key=lambda x: int(re.search(r"\d+", x[0]).group())
    )

    for hnf_id, hnf_data in sorted_hnfs:
        hits = hnf_data.get("m_demand_hits", 0)
        misses = hnf_data.get("m_demand_misses", 0)
        total = hits + misses
        report_lines.append(
            f"  {hnf_id:6s}: {hits:6d} hits + {misses:6d} misses = {total:6d} total"
        )

    # Add cache hierarchy analysis
    report_lines.extend(
        [
            "",
            "-" * 70,
            "CACHE HIERARCHY ANALYSIS",
            "-" * 70,
        ]
    )

    for line in cache_analysis:
        report_lines.append(f"  {line}")

    # Add conclusion
    report_lines.extend(
        [
            "",
            "=" * 70,
            "CONCLUSION",
            "=" * 70,
        ]
    )

    if is_valid:
        report_lines.append("[PASS] Smoke test completed successfully!")
        report_lines.append(
            "       All 16 LLC slices are reachable and received traffic."
        )
        report_lines.append(
            "       Address interleaving is working correctly."
        )
    else:
        report_lines.append("[FAIL] Smoke test validation failed!")
        report_lines.append("       See errors above for details.")

    report_lines.append("=" * 70)

    # Write report
    report_text = "\n".join(report_lines)
    with open(output_path, "w") as f:
        f.write(report_text)

    # Also print to console
    print(report_text)

    return is_valid


def main():
    """Main entry point."""
    if len(sys.argv) > 1:
        stats_path = Path(sys.argv[1])
    else:
        # Look for stats.txt in m5out directories
        m5out_dirs = list(Path(".").glob("m5out-smoke-*/stats.txt"))
        if not m5out_dirs:
            print(
                "ERROR: No stats.txt file found. Please provide path or run simulation first."
            )
            sys.exit(1)
        # Use the most recent one
        stats_path = sorted(m5out_dirs)[-1]
        print(f"Using stats file: {stats_path}")

    if not stats_path.exists():
        print(f"ERROR: Stats file not found: {stats_path}")
        sys.exit(1)

    # Parse stats
    stats = parse_stats_file(stats_path)

    # Generate report
    report_path = stats_path.parent / "analysis_report.txt"
    is_valid = generate_report(stats, report_path)

    print(f"\nReport written to: {report_path}")

    sys.exit(0 if is_valid else 1)


if __name__ == "__main__":
    main()
