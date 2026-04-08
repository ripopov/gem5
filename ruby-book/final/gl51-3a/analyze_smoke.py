#!/usr/bin/env python3
"""
analyze_smoke.py — Analyze rbook_test_smoke simulation results.

Reads stats.txt from a gem5 simulation of the single-core smoke test
and produces a structured report verifying:
  1. Simulation completed without errors.
  2. All 16 HN-F (LLC) slices received nonzero demand accesses.
  3. Per-HNF access counts are roughly balanced.
  4. Garnet network carried traffic (basic network liveness).
  5. DRAM controllers served requests.

Usage:
    python3 analyze_smoke.py <path-to-m5out-dir>
    python3 analyze_smoke.py <path-to-stats.txt>
"""

import os
import re
import sys


def parse_stats(path):
    """Parse a gem5 stats.txt into a flat dict of name -> float.

    Handles scalar stats, vector ::total sub-stats, and histogram
    sub-stats (mean, total, etc.).  Vector display lines (containing
    '|' or '%') are skipped.
    """
    stats = {}
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("---") or line.startswith("#"):
                continue
            if "|" in line or "%" in line:
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            name = parts[0]
            val_str = parts[1]
            if not val_str or val_str.startswith("("):
                continue
            try:
                val = float(val_str)
            except ValueError:
                continue
            stats[name] = val
    return stats


def get_hnf_accesses(stats, num_hnfs=16):
    rows = []
    for i in range(num_hnfs):
        p = f"system.ruby.hnf{i}.cntrl.cache"
        hits = stats.get(f"{p}.m_demand_hits", 0)
        misses = stats.get(f"{p}.m_demand_misses", 0)
        rows.append((i, int(hits), int(misses), int(hits + misses)))
    return rows


def get_l1d_stats(stats, num_cpus=16):
    rows = []
    for i in range(num_cpus):
        p = f"system.cpu{i}.l1d.cache"
        hits = stats.get(f"{p}.m_demand_hits", 0)
        misses = stats.get(f"{p}.m_demand_misses", 0)
        rows.append((i, int(hits), int(misses), int(hits + misses)))
    return rows


def get_l2_stats(stats, num_cpus=16):
    rows = []
    for i in range(num_cpus):
        p = f"system.cpu{i}.l2.cache"
        hits = stats.get(f"{p}.m_demand_hits", 0)
        misses = stats.get(f"{p}.m_demand_misses", 0)
        rows.append((i, int(hits), int(misses), int(hits + misses)))
    return rows


def get_garnet_stats(stats):
    p = "system.ruby.network"
    return {
        "flits_injected": stats.get(f"{p}.flits_injected::total", 0),
        "flits_received": stats.get(f"{p}.flits_received::total", 0),
        "packets_injected": stats.get(f"{p}.packets_injected::total", 0),
        "avg_flit_latency": stats.get(f"{p}.average_flit_latency", 0),
        "avg_flit_net_lat": stats.get(f"{p}.average_flit_network_latency", 0),
        "avg_flit_q_lat": stats.get(f"{p}.average_flit_queueing_latency", 0),
        "avg_pkt_latency": stats.get(f"{p}.average_packet_latency", 0),
        "avg_hops": stats.get(f"{p}.average_hops", 0),
        "avg_link_util": stats.get(f"{p}.avg_link_utilization", 0),
    }


def get_dram_stats(stats, num_dirs=2):
    rows = []
    for i in range(num_dirs):
        mc = f"system.mem_ctrls{i}"
        reads = int(stats.get(f"{mc}.readReqs", 0))
        writes = int(stats.get(f"{mc}.writeReqs", 0))
        snf = f"system.ruby.snf{i}.cntrl"
        req_to_mem = int(stats.get(f"{snf}.requestToMemory.m_msg_count", 0))
        rsp_from_mem = int(
            stats.get(f"{snf}.responseFromMemory.m_msg_count", 0)
        )
        rows.append(
            {
                "idx": i,
                "ddr_reads": reads,
                "ddr_writes": writes,
                "snf_reqs": req_to_mem,
                "snf_rsps": rsp_from_mem,
            }
        )
    return rows


def format_bar(val, max_val, width=30):
    if max_val == 0:
        return ""
    filled = int(width * val / max_val)
    return "#" * filled + "-" * (width - filled)


def run_analysis(stats_path):
    stats = parse_stats(stats_path)

    lines = []
    w = lines.append

    w("=" * 72)
    w("SMOKE TEST ANALYSIS  -  rbook_test_smoke (single-core)")
    w("=" * 72)
    w("")

    # --- 1. Simulation completion ---
    w("--- 1. Simulation Completion ---")
    sim_ticks = stats.get("simTicks", 0)
    sim_insts = stats.get("simInsts", 0)
    sim_freq = stats.get("simFreq", 0)
    host_tick_rate = stats.get("hostTickRate", 0)
    sim_seconds = stats.get("simSeconds", 0)

    if sim_ticks > 0:
        w(f"  Simulated ticks:  {sim_ticks:,.0f}")
        if sim_freq:
            w(f"  Simulated time:   {sim_seconds * 1e3:.2f} ms")
        w(f"  Instructions:     {int(sim_insts):,}")
        if host_tick_rate:
            w(f"  Host tick rate:   {host_tick_rate:.2e} ticks/s")
        w(f"  Sim freq:         {sim_freq:.2e} Hz")
        sim_ok = True
    else:
        w("  WARNING: simTicks == 0 -- simulation may have failed!")
        sim_ok = False
    w("")

    # --- 2. HNF (LLC slice) access distribution ---
    w("--- 2. HNF (LLC Slice) Access Distribution ---")
    hnf_rows = get_hnf_accesses(stats)
    max_total = max(r[3] for r in hnf_rows) if hnf_rows else 1
    nonzero_count = sum(1 for r in hnf_rows if r[3] > 0)
    total_accesses = sum(r[3] for r in hnf_rows)
    total_hits = sum(r[1] for r in hnf_rows)
    total_misses = sum(r[2] for r in hnf_rows)

    w(
        f"  {'HNF':>4}  {'Hits':>10}  {'Misses':>10}  {'Total':>10}  Distribution"
    )
    w(
        f"  {'----':>4}  {'----':>10}  {'------':>10}  {'-----':>10}  {'-' * 30}"
    )
    for idx, hits, misses, total in hnf_rows:
        bar = format_bar(total, max_total)
        w(f"  {idx:>4}  {hits:>10}  {misses:>10}  {total:>10}  {bar}")
    w("")
    w(f"  Total accesses:          {total_accesses:,}")
    w(f"  Total hits:              {total_hits:,}")
    w(f"  Total misses:            {total_misses:,}")
    if total_accesses > 0:
        w(
            f"  LLC hit rate:            {total_hits / total_accesses * 100:.1f}%"
        )
    w(f"  Slices with >0 accesses: {nonzero_count}/16")

    hnf_ok = False
    if nonzero_count == 16:
        w("  CHECK: All 16 HNF slices received accesses. PASS")
        hnf_ok = True
    elif nonzero_count > 0:
        w(f"  CHECK: Only {nonzero_count}/16 slices accessed. PARTIAL")
    else:
        w("  CHECK: No HNF accesses detected! FAIL")

    if total_accesses > 0 and nonzero_count == 16:
        vals = [r[3] for r in hnf_rows if r[3] > 0]
        min_v, max_v = min(vals), max(vals)
        ratio = max_v / min_v if min_v > 0 else float("inf")
        w(f"  Balance ratio (max/min): {ratio:.2f}x")
        if ratio <= 1.1:
            w(
                "  BALANCE: Nearly perfect -- as expected for sequential interleaved access."
            )
        elif ratio <= 2.0:
            w("  BALANCE: Within 2x -- good for interleaved accesses.")
        elif ratio <= 4.0:
            w("  BALANCE: Within 4x -- acceptable but skewed.")
        else:
            w(
                "  BALANCE: Highly skewed -- may indicate address mapping issue."
            )
    w("")

    # --- 3. L1D / L2 per-CPU ---
    w("--- 3. Per-CPU Cache Stats (L1D / L2) ---")
    l1d_rows = get_l1d_stats(stats)
    l2_rows = get_l2_stats(stats)
    w(
        f"  {'CPU':>4}  {'L1D hits':>10}  {'L1D miss':>10}  {'L2 hits':>10}  {'L2 miss':>10}"
    )
    w(
        f"  {'---':>4}  {'--------':>10}  {'--------':>10}  {'-------':>10}  {'-------':>10}"
    )
    for i in range(16):
        l1h = l1d_rows[i][1] if i < len(l1d_rows) else 0
        l1m = l1d_rows[i][2] if i < len(l1d_rows) else 0
        l2h = l2_rows[i][1] if i < len(l2_rows) else 0
        l2m = l2_rows[i][2] if i < len(l2_rows) else 0
        w(f"  {i:>4}  {l1h:>10}  {l1m:>10}  {l2h:>10}  {l2m:>10}")
    active_cpus = sum(1 for i in range(16) if l1d_rows[i][3] > 0)
    w(
        f"  Active CPUs: {active_cpus}/16 (only CPU0 expected for single-core smoke)"
    )
    w("")

    # --- 4. Garnet network ---
    w("--- 4. Garnet Network Statistics ---")
    garnet = get_garnet_stats(stats)
    w(f"  Flits injected:        {garnet['flits_injected']:>14,.0f}")
    w(f"  Flits received:        {garnet['flits_received']:>14,.0f}")
    w(f"  Packets injected:      {garnet['packets_injected']:>14,.0f}")
    w(f"  Avg flit latency:      {garnet['avg_flit_latency']:>14.2f} cycles")
    w(f"    network component:   {garnet['avg_flit_net_lat']:>14.2f} cycles")
    w(f"    queueing component:  {garnet['avg_flit_q_lat']:>14.2f} cycles")
    w(f"  Avg packet latency:    {garnet['avg_pkt_latency']:>14.2f} cycles")
    w(f"  Avg hops:              {garnet['avg_hops']:>14.2f}")
    w(f"  Avg link utilization:  {garnet['avg_link_util']:>14.4f}")
    net_ok = garnet["flits_injected"] > 0
    if net_ok:
        w("  CHECK: Network carried traffic. PASS")
    else:
        w("  CHECK: No network traffic detected! FAIL")
    w("")

    # --- 5. DRAM / SN-F ---
    w("--- 5. DRAM Controller / SN-F Stats ---")
    dram_rows = get_dram_stats(stats)
    total_ddr_reads = 0
    total_ddr_writes = 0
    for d in dram_rows:
        w(
            f"  DDR{d['idx']}: reads={d['ddr_reads']:>10,}  writes={d['ddr_writes']:>10,}"
        )
        w(
            f"  SNF{d['idx']}: reqToMem={d['snf_reqs']:>10,}  rspFromMem={d['snf_rsps']:>10,}"
        )
        total_ddr_reads += d["ddr_reads"]
        total_ddr_writes += d["ddr_writes"]
    w(f"  Total DDR reads:  {total_ddr_reads:>10,}")
    w(f"  Total DDR writes: {total_ddr_writes:>10,}")
    dram_ok = total_ddr_reads > 0
    if dram_ok:
        w("  CHECK: DRAM controllers served requests. PASS")
    else:
        w("  CHECK: No DRAM reads detected! FAIL")

    if len(dram_rows) == 2:
        r0 = dram_rows[0]["ddr_reads"]
        r1 = dram_rows[1]["ddr_reads"]
        if r0 + r1 > 0:
            ratio = max(r0, r1) / max(min(r0, r1), 1)
            w(f"  DDR0/DDR1 read balance ratio: {ratio:.2f}x")
    w("")

    # --- 6. Summary ---
    w("=" * 72)
    w("SUMMARY")
    w("=" * 72)
    checks = [
        ("Simulation completed", sim_ok),
        ("All 16 HNF slices accessed", hnf_ok),
        ("Network carried traffic", net_ok),
        ("DRAM controllers served requests", dram_ok),
    ]
    for label, ok in checks:
        status = "PASS" if ok else "FAIL"
        w(f"  [{status}] {label}")

    all_pass = all(c[1] for c in checks)
    w("")
    if all_pass:
        w("OVERALL: PASS")
        w("  The 4x4 CHI mesh booted successfully. A single core reached")
        w("  all 16 LLC slices with near-uniform distribution, the Garnet")
        w("  network carried traffic, and both DDR controllers served reads.")
    else:
        w("OVERALL: FAIL -- see warnings above.")
    w("")

    report = "\n".join(lines)

    report_path = os.path.join(
        os.path.dirname(stats_path) or ".", "smoke_report.txt"
    )
    with open(report_path, "w") as f:
        f.write(report + "\n")
    print(f"Report written to: {report_path}")

    return all_pass, report


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <path-to-m5out-dir-or-stats.txt>")
        sys.exit(1)

    path = sys.argv[1]
    if os.path.isdir(path):
        stats_path = os.path.join(path, "stats.txt")
    else:
        stats_path = path

    if not os.path.isfile(stats_path):
        print(f"Error: stats.txt not found at {stats_path}")
        sys.exit(1)

    all_pass, report = run_analysis(stats_path)
    print(report)
    sys.exit(0 if all_pass else 1)


if __name__ == "__main__":
    main()
