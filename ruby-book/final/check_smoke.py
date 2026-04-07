#!/usr/bin/env python3
"""
check_smoke.py — Analyse smoke-test stats and verify all 16 HN-F slices
were exercised with roughly balanced traffic.

Usage:
    python3 check_smoke.py <m5out-dir>
    python3 check_smoke.py m5out/rbook-smoke-20260407-234636
"""

import os
import re
import sys

NUM_HNFS = 16
# Maximum allowed deviation from the mean (fraction).  Accounts for
# instruction fetches, stack accesses, and alignment effects that make
# the distribution imperfect.
MAX_IMBALANCE = 0.20


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <m5out-dir>", file=sys.stderr)
        sys.exit(1)

    stats_path = os.path.join(sys.argv[1], "stats.txt")
    if not os.path.isfile(stats_path):
        print(f"ERROR: {stats_path} not found", file=sys.stderr)
        sys.exit(1)

    # Parse per-HN-F demand accesses
    pattern = re.compile(
        r"system\.ruby\.hnf(\d+)\.cntrl\.cache\.m_demand_accesses\s+(\d+)"
    )
    accesses = {}
    with open(stats_path) as f:
        for line in f:
            m = pattern.search(line)
            if m:
                accesses[int(m.group(1))] = int(m.group(2))

    # --- Table ---
    print(f"{'HN-F':>6}  {'Accesses':>10}")
    print(f"{'----':>6}  {'--------':>10}")
    for i in sorted(accesses):
        print(f"{i:>6}  {accesses[i]:>10}")

    total = sum(accesses.values())
    mean = total / len(accesses) if accesses else 0
    print(f"\n{'Total':>6}  {total:>10}")
    print(f"{'Mean':>6}  {mean:>10.1f}")

    # --- Self-checks ---
    errors = []

    # Check 1: all 16 HN-Fs present
    missing = set(range(NUM_HNFS)) - set(accesses.keys())
    if missing:
        errors.append(f"Missing HN-F slices: {sorted(missing)}")

    # Check 2: all nonzero
    zeros = [i for i in sorted(accesses) if accesses[i] == 0]
    if zeros:
        errors.append(f"HN-F slices with zero accesses: {zeros}")

    # Check 3: roughly balanced (within MAX_IMBALANCE of mean)
    if mean > 0:
        for i in sorted(accesses):
            dev = abs(accesses[i] - mean) / mean
            if dev > MAX_IMBALANCE:
                errors.append(
                    f"HN-F {i}: {accesses[i]} accesses, "
                    f"{dev:.1%} from mean ({mean:.0f}) — exceeds "
                    f"{MAX_IMBALANCE:.0%} threshold"
                )

    if errors:
        print("\nFAIL")
        for e in errors:
            print(f"  - {e}")
        sys.exit(1)
    else:
        print("\nPASS — all 16 HN-F slices nonzero and balanced")
        sys.exit(0)


if __name__ == "__main__":
    main()
