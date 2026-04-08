#!/usr/bin/env python3
"""
analyze_smoke.py — Analyze gem5 stats from the single-core smoke test (3a).

Parses the stats.txt file from a smoke-test simulation and checks:
  1. All 16 HN-F (LLC) slices received nonzero demand accesses.
  2. Access counts are roughly balanced (no slice >2x the mean).
  3. Both DRAM controllers received traffic.

Produces a summary report on stdout and writes it to summary_report.txt.

Usage:
    python3 analyze_smoke.py [path/to/m5out]

If no path is given, defaults to ./m5out in the same directory as this script.
"""

import os
import re
import sys
from collections import defaultdict


def parse_stats(stats_path):
    """Parse gem5 stats.txt into a dict of {stat_name: value_string}."""
    stats = {}
    with open(stats_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("---") or line.startswith("#"):
                continue
            # Lines with histogram bars (|) are multi-value; skip for scalar parse
            if "|" in line and "::" not in line:
                continue
            parts = line.split()
            if len(parts) >= 2:
                stats[parts[0]] = parts[1]
    return stats


def extract_hnf_stats(stats):
    """Extract per-HN-F demand hits, misses, and accesses."""
    hnf_data = {}
    for i in range(16):
        prefix = f"system.ruby.hnf{i}.cntrl.cache"
        hits = int(stats.get(f"{prefix}.m_demand_hits", 0))
        misses = int(stats.get(f"{prefix}.m_demand_misses", 0))
        accesses = int(stats.get(f"{prefix}.m_demand_accesses", 0))
        hnf_data[i] = {"hits": hits, "misses": misses, "accesses": accesses}
    return hnf_data


def extract_dram_stats(stats):
    """Extract per-DRAM-controller read/write request counts."""
    dram_data = {}
    for i in range(2):
        prefix = f"system.mem_ctrls{i}"
        reads = int(stats.get(f"{prefix}.readReqs", 0))
        writes = int(stats.get(f"{prefix}.writeReqs", 0))
        bytes_read = int(stats.get(f"{prefix}.bytesReadSys", 0))
        bytes_written = int(stats.get(f"{prefix}.bytesWrittenSys", 0))
        dram_data[i] = {
            "reads": reads,
            "writes": writes,
            "bytes_read": bytes_read,
            "bytes_written": bytes_written,
        }
    return dram_data


def extract_network_stats(stats):
    """Extract top-level Garnet network statistics."""
    net = {}
    net["avg_flit_latency"] = float(
        stats.get("system.ruby.network.average_flit_latency", 0)
    )
    net["avg_packet_latency"] = float(
        stats.get("system.ruby.network.average_packet_latency", 0)
    )
    net["total_flits"] = int(
        stats.get("system.ruby.network.flits_injected::total", 0)
    )
    net["total_packets"] = int(
        stats.get("system.ruby.network.packets_injected::total", 0)
    )
    return net


def extract_sim_stats(stats):
    """Extract top-level simulation statistics."""
    sim = {}
    sim["sim_seconds"] = float(stats.get("simSeconds", 0))
    sim["sim_ticks"] = int(stats.get("simTicks", 0))
    sim["sim_insts"] = int(stats.get("simInsts", 0))
    return sim


def extract_cpu_cache_stats(stats):
    """Extract CPU 0 cache stats for context."""
    cpu = {}
    for level in ["l1d", "l1i", "l2"]:
        prefix = f"system.cpu0.{level}.cache"
        hits = int(stats.get(f"{prefix}.m_demand_hits", 0))
        misses = int(stats.get(f"{prefix}.m_demand_misses", 0))
        accesses = int(stats.get(f"{prefix}.m_demand_accesses", 0))
        cpu[level] = {"hits": hits, "misses": misses, "accesses": accesses}
    return cpu


def analyze(m5out_dir):
    stats_path = os.path.join(m5out_dir, "stats.txt")
    if not os.path.exists(stats_path):
        print(f"ERROR: {stats_path} not found", file=sys.stderr)
        sys.exit(1)

    stats = parse_stats(stats_path)
    hnf = extract_hnf_stats(stats)
    dram = extract_dram_stats(stats)
    net = extract_network_stats(stats)
    sim = extract_sim_stats(stats)
    cpu = extract_cpu_cache_stats(stats)

    lines = []

    def out(s=""):
        lines.append(s)

    # --- Header ---
    out("=" * 72)
    out("  Smoke Test Analysis (Chapter 17, Stage 3a)")
    out("=" * 72)
    out()

    # --- Simulation overview ---
    out("## Simulation Overview")
    out(f"  Simulated time : {sim['sim_seconds']:.6f} s")
    out(f"  Simulated ticks: {sim['sim_ticks']:,}")
    out(f"  Instructions   : {sim['sim_insts']:,}")
    out()

    # --- CPU 0 cache hierarchy ---
    out("## CPU 0 Cache Hierarchy")
    out(
        f"  {'Level':<6} {'Hits':>10} {'Misses':>10} {'Accesses':>10} {'Hit Rate':>10}"
    )
    out(f"  {'-'*6:<6} {'-'*10:>10} {'-'*10:>10} {'-'*10:>10} {'-'*10:>10}")
    for level in ["l1i", "l1d", "l2"]:
        d = cpu[level]
        rate = (
            f"{100.0 * d['hits'] / d['accesses']:.1f}%"
            if d["accesses"] > 0
            else "N/A"
        )
        out(
            f"  {level.upper():<6} {d['hits']:>10,} {d['misses']:>10,}"
            f" {d['accesses']:>10,} {rate:>10}"
        )
    out()

    # --- HN-F (LLC) slice distribution ---
    out("## Per-HN-F (LLC Slice) Demand Accesses")
    out(
        f"  {'HN-F':>6} {'Hits':>10} {'Misses':>10} {'Accesses':>10} {'Hit Rate':>10}"
    )
    out(f"  {'-'*6:>6} {'-'*10:>10} {'-'*10:>10} {'-'*10:>10} {'-'*10:>10}")

    access_counts = []
    all_nonzero = True
    for i in range(16):
        d = hnf[i]
        access_counts.append(d["accesses"])
        if d["accesses"] == 0:
            all_nonzero = False
        rate = (
            f"{100.0 * d['hits'] / d['accesses']:.1f}%"
            if d["accesses"] > 0
            else "N/A"
        )
        out(
            f"  hnf{i:<3} {d['hits']:>10,} {d['misses']:>10,}"
            f" {d['accesses']:>10,} {rate:>10}"
        )

    total_accesses = sum(access_counts)
    mean_accesses = total_accesses / 16 if total_accesses > 0 else 0
    min_accesses = min(access_counts)
    max_accesses = max(access_counts)
    spread = (
        (max_accesses - min_accesses) / mean_accesses * 100
        if mean_accesses > 0
        else 0
    )

    out()
    out(f"  Total accesses : {total_accesses:,}")
    out(f"  Mean per slice : {mean_accesses:,.1f}")
    out(f"  Min / Max      : {min_accesses:,} / {max_accesses:,}")
    out(f"  Spread (max-min)/mean: {spread:.2f}%")
    out()

    # --- Balance check ---
    balanced = all(
        a > 0 and a < 2 * mean_accesses and a > 0.5 * mean_accesses
        for a in access_counts
    )

    out("## LLC Reachability Check")
    if all_nonzero:
        out("  [PASS] All 16 HN-F slices received nonzero demand accesses.")
    else:
        zero_slices = [i for i in range(16) if access_counts[i] == 0]
        out(f"  [FAIL] HN-F slices with zero accesses: {zero_slices}")
        out("         This indicates a wiring or address-mapping error.")

    if balanced:
        out(
            f"  [PASS] Accesses are well-balanced (spread {spread:.2f}%"
            f" of mean)."
        )
    else:
        out(
            f"  [WARN] Access distribution is unbalanced (spread"
            f" {spread:.2f}% of mean)."
        )
        out("         Some slices may have >2x or <0.5x the mean.")
    out()

    # --- DRAM controller balance ---
    out("## DRAM Controller Balance")
    for i in range(2):
        d = dram[i]
        out(
            f"  DDR{i} (router {'0' if i == 0 else '15'}):"
            f" {d['reads']:,} reads, {d['writes']:,} writes,"
            f" {d['bytes_read']:,} bytes read"
        )

    total_reads = dram[0]["reads"] + dram[1]["reads"]
    if total_reads > 0:
        ratio = dram[0]["reads"] / total_reads * 100
        out(f"  DDR0 share: {ratio:.1f}% of total reads")
        dram_balanced = 40 < ratio < 60
        if dram_balanced:
            out("  [PASS] DRAM traffic is well-balanced between controllers.")
        else:
            out(
                f"  [WARN] DRAM traffic is skewed"
                f" ({ratio:.1f}% / {100 - ratio:.1f}%)."
            )
    out()

    # --- Garnet network summary ---
    out("## Garnet Network Summary")
    out(f"  Total flits injected : {net['total_flits']:,}")
    out(f"  Total packets injected: {net['total_packets']:,}")
    out(f"  Avg flit latency     : {net['avg_flit_latency']:.1f} cycles")
    out(f"  Avg packet latency   : {net['avg_packet_latency']:.1f} cycles")
    out()

    # --- Overall verdict ---
    out("=" * 72)
    if all_nonzero and balanced:
        out("  OVERALL: PASS")
        out("  The 4x4 CHI mesh is correctly wired.  All 16 LLC slices")
        out(
            "  are reachable and address interleaving distributes"
            " traffic evenly."
        )
    elif all_nonzero:
        out("  OVERALL: PASS (with balance warning)")
        out("  All slices are reachable but distribution is uneven.")
    else:
        out("  OVERALL: FAIL")
        out(
            "  Some LLC slices received no traffic — check router"
            " bindings and address mapping."
        )
    out("=" * 72)

    return "\n".join(lines)


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    if len(sys.argv) > 1:
        m5out_dir = sys.argv[1]
    else:
        m5out_dir = os.path.join(script_dir, "m5out")

    report = analyze(m5out_dir)
    print(report)

    # Write report to file alongside the script
    report_path = os.path.join(script_dir, "summary_report.txt")
    with open(report_path, "w") as f:
        f.write(report + "\n")
    print(f"\nReport written to {report_path}")


if __name__ == "__main__":
    main()
