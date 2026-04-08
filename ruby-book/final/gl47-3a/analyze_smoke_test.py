#!/usr/bin/env python3
"""
Analyze gem5 statistics for the smoke test.

This script reads gem5's stats.txt file and generates a summary report for the
single-core smoke test, verifying that all 16 LLC slices were accessed and the
distribution is balanced.
"""

import argparse
import os
import re
import sys
from collections import defaultdict
from pathlib import Path


def read_stats_file(stats_path):
    """Parse gem5 stats.txt file into a dictionary."""
    stats = {}
    with open(stats_path) as f:
        for line in f:
            # Skip comments and empty lines
            if line.startswith("#") or not line.strip():
                continue

            # Match statistics lines: name value [units] [description]
            match = re.match(
                r"^(\S+)\s+(-?\d+\.?\d*(?:[eE][+-]?\d+)?)\s*", line
            )
            if match:
                name = match.group(1)
                value = match.group(2)

                # Try to parse as float, fall back to string
                try:
                    stats[name] = float(value)
                except ValueError:
                    stats[name] = value
    return stats


def extract_hnf_stats(stats):
    """
    Extract per-HN-F (LLC slice) statistics.

    HN-F controllers are named: system.ruby.hnf{i}.cntrl
    where i ranges from 0 to 15 for our 16 HN-F nodes.
    """
    hnf_stats = {}

    for controller_idx in range(16):
        controller_name = f"system.ruby.hnf{controller_idx}.cntrl"

        # Demand accesses (hits + misses)
        hits_key = f"{controller_name}.cache.m_demand_hits"
        misses_key = f"{controller_name}.cache.m_demand_misses"

        hits = stats.get(hits_key, 0)
        misses = stats.get(misses_key, 0)
        total_accesses = hits + misses

        hnf_stats[controller_idx] = {
            "name": controller_name,
            "hits": hits,
            "misses": misses,
            "total": total_accesses,
            "hit_rate": hits / total_accesses if total_accesses > 0 else 0.0,
        }

    return hnf_stats


def extract_dram_stats(stats):
    """
    Extract DRAM controller statistics.

    Memory controllers are named: system.mem_ctrls0 and system.mem_ctrls1
    for DDR0 and DDR1 respectively.
    """
    dram_stats = {}

    # DDR0: mem_ctrls0
    ddr0_name = "system.mem_ctrls0"
    dram_stats[0] = {
        "name": "DDR0 (router 0)",
        "controller_name": ddr0_name,
        "readReqs": stats.get(f"{ddr0_name}.readReqs", 0),
        "writeReqs": stats.get(f"{ddr0_name}.writeReqs", 0),
    }

    # DDR1: mem_ctrls1
    ddr1_name = "system.mem_ctrls1"
    dram_stats[1] = {
        "name": "DDR1 (router 15)",
        "controller_name": ddr1_name,
        "readReqs": stats.get(f"{ddr1_name}.readReqs", 0),
        "writeReqs": stats.get(f"{ddr1_name}.writeReqs", 0),
    }

    return dram_stats


def extract_cpu_stats(stats):
    """Extract CPU 0 statistics."""
    cpu_stats = {
        "name": "CPU 0 (smoke test core)",
        "committed_insts": stats.get("system.cpu0.commitStats0.numInsts", 0),
        "num_cycles": stats.get("system.cpu0.numCycles", 0),
        "icache_hits": stats.get("system.cpu0.l1i.cache.m_demand_hits", 0),
        "icache_misses": stats.get("system.cpu0.l1i.cache.m_demand_misses", 0),
        "dcache_hits": stats.get("system.cpu0.l1d.cache.m_demand_hits", 0),
        "dcache_misses": stats.get("system.cpu0.l1d.cache.m_demand_misses", 0),
    }

    # Calculate hit rates
    total_icache = cpu_stats["icache_hits"] + cpu_stats["icache_misses"]
    total_dcache = cpu_stats["dcache_hits"] + cpu_stats["dcache_misses"]
    cpu_stats["icache_hit_rate"] = (
        cpu_stats["icache_hits"] / total_icache if total_icache > 0 else 0.0
    )
    cpu_stats["dcache_hit_rate"] = (
        cpu_stats["dcache_hits"] / total_dcache if total_dcache > 0 else 0.0
    )

    return cpu_stats


def extract_garnet_stats(stats):
    """Extract Garnet network statistics."""
    garnet_stats = {
        "total_flits": stats.get(
            "system.ruby.network.total_external_flits", 0
        ),
        "total_packets": stats.get("system.ruby.network.total_packets", 0),
        "avg_link_latency": 0.0,
    }

    # Calculate average link utilization if stats are available
    # This is approximate - actual analysis would require per-link stats
    return garnet_stats


def print_hnf_report(hnf_stats):
    """Print HN-F (LLC slice) access distribution."""
    print("\n" + "=" * 80)
    print("LLC Slice (HN-F) Access Distribution")
    print("=" * 80)

    # Calculate total accesses
    total_accesses = sum(h["total"] for h in hnf_stats.values())
    avg_accesses = total_accesses / 16 if total_accesses > 0 else 0

    print(f"\nTotal accesses across all LLC slices: {int(total_accesses)}")
    print(f"Average per slice: {avg_accesses:.2f}")
    print()

    # Print table header
    print(
        f"{'Slice':<8} {' accesses':<12} {'Hits':<12} {'Misses':<12} {'Hit Rate':<10} {'% of Total':<12}"
    )
    print("-" * 80)

    # Print each slice
    for idx in sorted(hnf_stats.keys()):
        s = hnf_stats[idx]
        pct = (
            (s["total"] / total_accesses * 100) if total_accesses > 0 else 0.0
        )
        print(
            f"  HN-F {idx:<3} {int(s['total']):<12} {int(s['hits']):<12} {int(s['misses']):<12} {s['hit_rate']:.4f} {pct:>9.2f}%"
        )

    # Check for slices with zero accesses
    zero_access_slices = [
        idx for idx, s in hnf_stats.items() if s["total"] == 0
    ]
    if zero_access_slices:
        print(
            f"\n⚠️  WARNING: Slices with zero accesses: {zero_access_slices}"
        )
        print(
            "   This indicates a problem with address mapping or router binding."
        )
    else:
        print(f"\n✓ All 16 LLC slices were accessed")

    # Check balance
    total_misses = sum(h["misses"] for h in hnf_stats.values())
    if total_accesses > 0:
        std_dev = (
            sum((h["total"] - avg_accesses) ** 2 for h in hnf_stats.values())
            / 16
        ) ** 0.5
        cv = (std_dev / avg_accesses) if avg_accesses > 0 else 0
        print(
            f"Balance check: std_dev = {std_dev:.2f}, coeff. of variation = {cv:.4f}"
        )

        if cv < 0.05:
            print(f"✓ Access distribution is well balanced (CV < 5%)")
        elif cv < 0.10:
            print(f"⚠️  Access distribution has moderate imbalance (CV < 10%)")
        else:
            print(
                f"✗ Access distribution is significantly imbalanced (CV >= 10%)"
            )


def print_dram_report(dram_stats):
    """Print DRAM controller statistics."""
    print("\n" + "=" * 80)
    print("DRAM Controller Statistics")
    print("=" * 80)

    total_reads = dram_stats[0]["readReqs"] + dram_stats[1]["readReqs"]
    total_writes = dram_stats[0]["writeReqs"] + dram_stats[1]["writeReqs"]

    print(f"\nTotal read requests: {int(total_reads)}")
    print(f"Total write requests: {int(total_writes)}")
    print()

    print(
        f"{'Controller':<20} {'Read Reqs':<12} {'Write Reqs':<12} {'% of Reads':<12}"
    )
    print("-" * 60)

    for dram_id in [0, 1]:
        d = dram_stats[dram_id]
        read_pct = (
            (d["readReqs"] / total_reads * 100) if total_reads > 0 else 0.0
        )
        print(
            f"{d['name']:<20} {int(d['readReqs']):<12} {int(d['writeReqs']):<12} {read_pct:>10.2f}%"
        )

    # Check balance
    if total_reads > 0:
        imbalance = (
            abs(dram_stats[0]["readReqs"] - dram_stats[1]["readReqs"])
            / total_reads
        )
        if imbalance < 0.05:
            print(f"\n✓ DRAM load is well balanced (imbalance < 5%)")
        else:
            print(
                f"\n⚠️  DRAM imbalance: {imbalance * 100:.2f}% (difference > 5%)"
            )


def print_cpu_report(cpu_stats):
    """Print CPU statistics."""
    print("\n" + "=" * 80)
    print(f"CPU Statistics ({cpu_stats['name']})")
    print("=" * 80)

    print(f"\nCommitted instructions: {int(cpu_stats['committed_insts'])}")
    print(f"Simulation cycles: {int(cpu_stats['num_cycles'])}")

    if cpu_stats["num_cycles"] > 0:
        ipc = cpu_stats["committed_insts"] / cpu_stats["num_cycles"]
        print(f"IPC: {ipc:.4f}")

    print(
        f"\nI-cache: {int(cpu_stats['icache_hits'])} hits, {int(cpu_stats['icache_misses'])} misses, "
        f"hit rate = {cpu_stats['icache_hit_rate']:.4f}"
    )
    print(
        f"D-cache: {int(cpu_stats['dcache_hits'])} hits, {int(cpu_stats['dcache_misses'])} misses, "
        f"hit rate = {cpu_stats['dcache_hit_rate']:.4f}"
    )


def print_garnet_report(garnet_stats):
    """Print Garnet network statistics."""
    print("\n" + "=" * 80)
    print("Garnet Network Statistics")
    print("=" * 80)

    print(f"\nTotal external flits: {int(garnet_stats['total_flits'])}")
    print(f"Total packets: {int(garnet_stats['total_packets'])}")


def generate_summary(stats, hnf_stats, dram_stats, cpu_stats):
    """Generate a summary verdict."""
    print("\n" + "=" * 80)
    print("SUMMARY")
    print("=" * 80)

    issues = []

    # Check that all slices were accessed
    zero_access_slices = [
        idx for idx, s in hnf_stats.items() if s["total"] == 0
    ]
    if zero_access_slices:
        issues.append(f"Slices with zero accesses: {zero_access_slices}")

    # Check LLCH balance
    total_accesses = sum(h["total"] for h in hnf_stats.values())
    avg_accesses = total_accesses / 16 if total_accesses > 0 else 0
    if total_accesses > 0:
        std_dev = (
            sum((h["total"] - avg_accesses) ** 2 for h in hnf_stats.values())
            / 16
        ) ** 0.5
        cv = (std_dev / avg_accesses) if avg_accesses > 0 else 0
        if cv >= 0.10:
            issues.append(
                f"LLC slices significantly imbalanced (CV = {cv:.4f})"
            )

    # Check DRAM balance
    total_reads = dram_stats[0]["readReqs"] + dram_stats[1]["readReqs"]
    if total_reads > 0:
        imbalance = (
            abs(dram_stats[0]["readReqs"] - dram_stats[1]["readReqs"])
            / total_reads
        )
        if imbalance >= 0.10:
            issues.append(f"DRAM imbalance > 10% ({imbalance * 100:.2f}%)")

    # Print verdict
    if not issues:
        print("✓ SMOKE TEST PASSED")
        print("  - All 16 LLC slices were accessed")
        print("  - LLCH access distribution is balanced")
        print("  - DRAM load is balanced")
        return 0
    else:
        print("✗ SMOKE TEST FAILED")
        for issue in issues:
            print(f"  - {issue}")
        return 1


def main():
    parser = argparse.ArgumentParser(
        description="Analyze gem5 statistics for the smoke test"
    )
    parser.add_argument(
        "stats_file", type=str, help="Path to gem5 stats.txt file"
    )
    parser.add_argument(
        "-o",
        "--output",
        type=str,
        help="Write report to file instead of stdout",
    )
    args = parser.parse_args()

    # Check if stats file exists
    if not os.path.exists(args.stats_file):
        print(
            f"Error: stats file not found: {args.stats_file}", file=sys.stderr
        )
        return 1

    # Read and parse statistics
    stats = read_stats_file(args.stats_file)

    # Extract relevant statistics
    hnf_stats = extract_hnf_stats(stats)
    dram_stats = extract_dram_stats(stats)
    cpu_stats = extract_cpu_stats(stats)
    garnet_stats = extract_garnet_stats(stats)

    # Output report
    if args.output:
        with open(args.output, "w") as f:
            original_stdout = sys.stdout
            sys.stdout = f

            print("=" * 80)
            print(f"SMOKE TEST ANALYSIS REPORT")
            print(f"Statistics file: {args.stats_file}")
            print("=" * 80)

            print_cpu_report(cpu_stats)
            print_hnf_report(hnf_stats)
            print_dram_report(dram_stats)
            print_garnet_report(garnet_stats)

            exit_code = generate_summary(
                stats, hnf_stats, dram_stats, cpu_stats
            )

            sys.stdout = original_stdout
        print(f"Report written to: {args.output}")
    else:
        print("=" * 80)
        print(f"SMOKE TEST ANALYSIS REPORT")
        print(f"Statistics file: {args.stats_file}")
        print("=" * 80)

        print_cpu_report(cpu_stats)
        print_hnf_report(hnf_stats)
        print_dram_report(dram_stats)
        print_garnet_report(garnet_stats)

        exit_code = generate_summary(stats, hnf_stats, dram_stats, cpu_stats)

    return exit_code


if __name__ == "__main__":
    sys.exit(main())
