#!/usr/bin/env python3
"""
protocoltrace_latency.py -- Extract CHI read-transaction latencies from a gem5
ProtocolTrace (debug flags: ProtocolTrace,RubyNetwork).

Usage:
    # ReadUnique (store / partial-write workload) -- the defaults
    python protocoltrace_latency.py trace.gz [--roi-start-clk N] [--roi-end-clk N]

    # ReadShared (load workload) -- override the event names
    python protocoltrace_latency.py trace.gz --roi-start-clk 24379 \
        --start-event SendReadShared --hnf-event ReadShared \
        --data-event CompData_SD_PD

The input may be a .gz or a plain text trace. --roi-start-clk / --roi-end-clk
restrict to transactions completing in a clock window (e.g. the ROI stats
window) so results match a given stats.txt dump. The transaction is defined by
--start-event / --hnf-event / --data-event / --data-beats (defaults below).

Outputs (written to the current working dir):
    trace_transactions.csv   one row per transaction
    trace_stats.txt          per-RNF latency summary (matches stats.txt
                             outTransLatHist.<start-event>)

Latency model (all in Ruby clock cycles; trace timestamps are in ticks):
    START         start-event  registered at the requesting RNF cache
    HNF arrival   hnf-event    registered at the home node (HNF)
    END           data-beats-th data-event registered back at the RNF cache
    LATENCY_TO_HNF = HNF_arrival - START      (request leg)
    LATENCY_CLK    = END          - START      (full outTransLatHist interval)

Only ProtocolTrace rows are consumed; RubyNetwork flit rows are ignored.
Event defaults live in the CONSTANTS block below.
"""

import argparse
import csv
import gzip
import re
import sys
from collections import defaultdict

# ----------------------------------------------------------------------------
# CONSTANTS -- edit these to retarget the script.
# ----------------------------------------------------------------------------

# Ruby clock period in ticks (system.clk_domain.clock in config.ini). One cycle
# = CLOCK_PERIOD ticks; every trace timestamp is divided by this to get clocks.
CLOCK_PERIOD = 500

# Controller version numbering in the ProtocolTrace.  Each mesh tile hosts one
# RNF and one HNF; RNF versions are [0, HNF_VERSION_BASE), HNF versions start at
# HNF_VERSION_BASE.  HNF_ID = version - HNF_VERSION_BASE.
HNF_VERSION_BASE = 16

# Default transaction event names (SLICC transition labels, column 4). All
# four are overridable on the command line so the script retargets to any CHI
# transaction without edits. Defaults describe a store's ReadUnique; for a
# load pass: --start-event SendReadShared --hnf-event ReadShared
#            --data-event CompData_SD_PD
START_EVENT = "SendReadUnique"   # logged at the RNF  -> transaction START
HNF_EVENT = "ReadUnique_PoC"     # logged at the HNF  -> request reached home
DATA_EVENT = "CompData_UD_PD"    # logged at the RNF  -> a data beat arrived
DATA_BEATS = 2                   # END = arrival of this many DATA_EVENTs

# Controller object token (column 3) -- distinguishes protocol rows.
CTRL_TOKEN = "Cache"

# Matches a ProtocolTrace row, capturing: tick, version, event, address.
#   "   6989000   3   Cache   SendReadUnique BUSY_BLKD>BUSY_INTR [0x30000, ..."
PROTO_RE = re.compile(
    r"^\s+(\d+)\s+(\d+)\s+" + CTRL_TOKEN + r"\s+(\S+)\s+\S+\s+\[(0x[0-9a-fA-F]+),"
)

CSV_FIELDS = [
    "RNF_ID", "HNF_ID", "ADDRESS",
    "START_CLK", "END_CLK", "LATENCY_TO_HNF", "LATENCY_CLK",
]

OUT_CSV = "trace_transactions.csv"
OUT_STATS = "trace_stats.txt"


# ----------------------------------------------------------------------------
# Parsing
# ----------------------------------------------------------------------------

def open_trace(path):
    """Open a plain or gzip-compressed trace as a text stream."""
    if path.endswith(".gz"):
        return gzip.open(path, "rt", encoding="latin-1")
    return open(path, "rt", encoding="latin-1")


def parse_events(path, start_event, hnf_event, data_event):
    """Return {address: [(tick, version, event), ...]} for relevant events.

    Only START/HNF/DATA events are kept.  ProtocolTrace rows are indented with
    leading whitespace; RubyNetwork rows start with a digit, so a cheap
    first-char test skips the bulk of the (huge) network trace before regex.
    """
    wanted = {start_event, hnf_event, data_event}
    by_addr = defaultdict(list)
    with open_trace(path) as fh:
        for line in fh:
            if not line[:1].isspace():
                continue
            m = PROTO_RE.match(line)
            if not m:
                continue
            event = m.group(3)
            if event not in wanted:
                continue
            tick = int(m.group(1))
            version = int(m.group(2))
            by_addr[m.group(4)].append((tick, version, event))
    return by_addr


def build_transactions(by_addr, start_event, hnf_event, data_event,
                       data_beats):
    """Pair events into transactions via a per-address time-ordered walk.

    Each START_EVENT opens a transaction; the first following HNF_EVENT records
    the HNF arrival, and the data_beats-th following DATA_EVENT closes it.
    """
    txns = []
    for addr, events in by_addr.items():
        events.sort()  # by tick, then version, then event
        pending = None
        for tick, version, event in events:
            is_rnf = version < HNF_VERSION_BASE
            if event == start_event and is_rnf:
                pending = {
                    "rnf": version, "addr": addr, "start": tick,
                    "hnf": None, "hnf_tick": None, "beats": 0, "end": None,
                }
            elif pending is None:
                continue
            elif event == hnf_event and not is_rnf and pending["hnf"] is None:
                pending["hnf"] = version - HNF_VERSION_BASE
                pending["hnf_tick"] = tick
            elif event == data_event and version == pending["rnf"]:
                pending["beats"] += 1
                if pending["beats"] == data_beats:
                    pending["end"] = tick
                    txns.append(pending)
                    pending = None
    return txns


# ----------------------------------------------------------------------------
# Output
# ----------------------------------------------------------------------------

def to_clk(ticks):
    """Convert ticks to clock cycles (exact: ticks are CLOCK_PERIOD multiples)."""
    q, r = divmod(ticks, CLOCK_PERIOD)
    return q if r == 0 else ticks / CLOCK_PERIOD


def make_rows(txns, roi_start_clk, roi_end_clk):
    """Build CSV rows for transactions that completed in the ROI window.

    A transaction is included iff its END (completion) clock lies in the
    window -- this mirrors gem5's outTransLatHist, which records a sample at
    completion. The HNF leg is optional: if the home-node event was not traced
    (e.g. a read served by a snoop rather than the HNF), HNF_ID and
    LATENCY_TO_HNF are left blank but the transaction still counts.
    """
    rows = []
    for t in txns:
        if t["end"] is None:
            continue
        end_clk = to_clk(t["end"])
        if end_clk < roi_start_clk or end_clk > roi_end_clk:
            continue
        has_hnf = t["hnf"] is not None
        rows.append({
            "RNF_ID": t["rnf"],
            "HNF_ID": t["hnf"] if has_hnf else "",
            "ADDRESS": t["addr"],
            "START_CLK": to_clk(t["start"]),
            "END_CLK": end_clk,
            "LATENCY_TO_HNF":
                to_clk(t["hnf_tick"] - t["start"]) if has_hnf else "",
            "LATENCY_CLK": to_clk(t["end"] - t["start"]),
        })
    return rows


def write_csv(rows, path):
    rows.sort(key=lambda r: (r["RNF_ID"], r["START_CLK"]))
    with open(path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=CSV_FIELDS)
        w.writeheader()
        w.writerows(rows)


def _hnf_label(row):
    """'hnf<id>' for a transaction row, or 'hnf?' if the HNF leg is unknown."""
    return "hnf%s" % row["HNF_ID"] if row["HNF_ID"] != "" else "hnf?"


def write_stats(rows, path, roi_start_clk, roi_end_clk, start_event):
    by_rnf = defaultdict(list)
    for r in rows:
        by_rnf[r["RNF_ID"]].append(r)

    end_txt = "inf" if roi_end_clk == float("inf") else str(roi_end_clk)
    with open(path, "w") as fh:
        fh.write("%s latency per RNF (clocks)\n" % start_event)
        fh.write("ROI window (END clk): [%s, %s]\n" % (roi_start_clk, end_txt))
        fh.write("=" * 72 + "\n\n")
        for rnf in sorted(by_rnf):
            rs = by_rnf[rnf]
            lats = [r["LATENCY_CLK"] for r in rs]
            lo = min(rs, key=lambda r: r["LATENCY_CLK"])
            hi = max(rs, key=lambda r: r["LATENCY_CLK"])
            fh.write("system.ruby.rnf%d.cntrl  %s\n" % (rnf, start_event))
            fh.write("  count   : %d\n" % len(rs))
            fh.write("  mean    : %.6f\n" % (sum(lats) / len(lats)))
            fh.write("  min     : %s clk  (addr %s @%s -> %s)\n"
                     % (lo["LATENCY_CLK"], lo["ADDRESS"],
                        lo["START_CLK"], _hnf_label(lo)))
            fh.write("  max     : %s clk  (addr %s @%s -> %s)\n"
                     % (hi["LATENCY_CLK"], hi["ADDRESS"],
                        hi["START_CLK"], _hnf_label(hi)))
            fh.write("\n")


# ----------------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------------

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace", help="ProtocolTrace file (.gz or plain text)")
    ap.add_argument("--roi-start-clk", type=int, default=0,
                    help="keep transactions ending at/after this clock")
    ap.add_argument("--roi-end-clk", type=int, default=None,
                    help="keep transactions ending at/before this clock")
    ap.add_argument("--start-event", default=START_EVENT,
                    help="RNF transition that starts a transaction "
                         "(default: %(default)s)")
    ap.add_argument("--hnf-event", default=HNF_EVENT,
                    help="HNF transition marking request arrival "
                         "(default: %(default)s)")
    ap.add_argument("--data-event", default=DATA_EVENT,
                    help="RNF transition for each returning data beat "
                         "(default: %(default)s)")
    ap.add_argument("--data-beats", type=int, default=DATA_BEATS,
                    help="data beats that complete a transaction "
                         "(default: %(default)s)")
    args = ap.parse_args(argv)

    roi_start = args.roi_start_clk
    roi_end = args.roi_end_clk if args.roi_end_clk is not None else float("inf")

    print("parsing %s (start=%s hnf=%s data=%s x%d) ..."
          % (args.trace, args.start_event, args.hnf_event, args.data_event,
             args.data_beats), file=sys.stderr)
    by_addr = parse_events(args.trace, args.start_event, args.hnf_event,
                           args.data_event)
    txns = build_transactions(by_addr, args.start_event, args.hnf_event,
                              args.data_event, args.data_beats)
    rows = make_rows(txns, roi_start, roi_end)
    print("transactions: %d total, %d in ROI window"
          % (len(txns), len(rows)), file=sys.stderr)

    write_csv(rows, OUT_CSV)
    write_stats(rows, OUT_STATS, roi_start, roi_end, args.start_event)
    print("wrote %s and %s" % (OUT_CSV, OUT_STATS), file=sys.stderr)


if __name__ == "__main__":
    main(sys.argv[1:])
