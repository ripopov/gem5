#!/usr/bin/env python3
"""
garnet_trace.py -- Cycle-by-cycle trace of ONE CHI read transaction across the
Garnet network, reconstructed from a gem5 trace captured with
`--debug-flags=ProtocolTrace,RubyNetwork`.

This tool zooms into a *single* transaction (selected by address) and
reconstructs its full timeline in three phases:

    Phase 1  request   RNF -> HNF   (per-router network hops)
    Phase 2  HNF       internal controller events (L3 lookup / data read)
    Phase 3  data      HNF -> RNF   (per-router network hops)

To keep the per-hop log readable, exactly ONE flit is followed per network leg:
the request flit (Phase 1) and the *completing* data beat (Phase 3). A read
returns DATA_BEATS data beats; only the last one (whose arrival is END) is
routed hop-by-hop -- the earlier beats take the same route and are omitted. All
controller-side ProtocolTrace events (including every beat's arrival) are kept.

Usage:
    python garnet_trace.py /path/to/trace.gz <addr> [start_cycle]

    <addr>        cache-line address, e.g. 0x9a80 (any hex form accepted)
    [start_cycle] optional Ruby clock of the transaction's START event; use it
                  to disambiguate when an address is touched more than once.
                  If omitted, the FIRST read transaction at <addr> is used.

Example:
    python garnet_trace.py trace.gz 0x31c0
    python garnet_trace.py trace.gz 0x9a80 25976

Output (cwd):  trace_<type>_<addr>.txt   e.g. trace_ReadUnique_0x9a80.txt
Fixed-width columns:  Clk  Tick  DeltaClk  TotalClk  EventDescription

How a transaction is identified (read-agnostic -- works for ReadUnique,
ReadShared, ReadOnce, ... without flags):
  * START : first RNF (version < 16) ProtocolTrace event matching `SendRead*`
            for <addr> (at/after start_cycle if given).
  * HNF   : first HNF (version >= 16) `Read*` event in the START..END window
            -- the request-leg arrival (e.g. ReadUnique_PoC).
  * END   : the DATA_BEATS-th `CompData*` event back at the RNF (== RNF ver).

Network flits carry no address on their per-hop rows -- only a PacketId. We
bridge address <-> PacketId via the NetworkInterface "Scheduling" (injection)
rows, which carry BOTH the PacketId and the full Message (addr + type). Within
the transaction's time window we pick:
  * the request flit : Scheduling whose Message type == <read>  (START minus
                       "Send"), giving PacketId + Src/Dest routers (RNF/HNF).
  * the data  flit   : the last of the DATA_BEATS `CompData*` Schedulings
                       injected by the HNF router (the completing beat).
Each PacketId is then followed hop-by-hop through its `Consuming` (flit lands
in a router input) and `SwitchAllocator ... granted` (wins the switch) rows.

Two passes over the trace: pass 1 collects address-filtered ProtocolTrace +
Scheduling rows and selects the transaction; pass 2 follows the chosen
PacketIds through the routers. Both pre-filter cheaply before any regex.
"""

from __future__ import annotations

import gzip
import re
import sys
from dataclasses import dataclass
from typing import NamedTuple, Optional, TextIO

# ----------------------------------------------------------------------------
# CONSTANTS
# ----------------------------------------------------------------------------

# Ruby clock period in ticks (system.clk_domain.clock). clk = tick / this.
CLOCK_PERIOD = 500

# Controller version numbering in ProtocolTrace: RNF = [0, BASE), HNF >= BASE.
HNF_VERSION_BASE = 16

# Beats that complete a read (CompData_*); END = arrival of this many at RNF.
DATA_BEATS = 2

# Fixed-width output row: 4 left-aligned numeric columns + free-text event.
ROW_FMT = "%-8s  %-11s  %-8s  %-8s  %s"

# Read-transaction event wildcards (matched against the ProtocolTrace event
# column). Read-agnostic by design -- no per-transaction flags needed.
START_RE = re.compile(r"^SendRead\w*$")  # RNF: SendReadUnique / SendReadShared
HNF_RE = re.compile(r"^Read\w*$")        # HNF: ReadUnique_PoC / ReadShared ...
DATA_RE = re.compile(r"^CompData\w*$")   # RNF: CompData_UD_PD / CompData_SD_PD

# Garnet router structural pipeline (cycles) -- consume->SA-grant beyond this
# is queueing. Used only to annotate per-hop rows.
ROUTER_PIPELINE_CY = 3

# --- Row formats ------------------------------------------------------------
# ProtocolTrace (indented, no ':' after tick):
#   "  13035000  26  Cache  ReadUnique_PoC  UD>BUSY_BLKD [0x9a80, line ...]"
PROTO_RE = re.compile(
    r"^\s+(\d+)\s+(\d+)\s+Cache\s+(\S+)\s+(\S+)\s+"
    r"\[(0x[0-9a-fA-F]+),[^\]]*\](.*)$"
)
# RubyNetwork rows start with "<tick>: ".
TICK_RE = re.compile(r"^\s*(\d+):")
PKTID_RE = re.compile(r"PacketId=(\d+)")
SCHED_RE = re.compile(r"Scheduling at ")
SCHED_VNET_RE = re.compile(r"Vnet=(\d+)")
SCHED_SRC_RE = re.compile(r"Src Router=(\d+)")
SCHED_DST_RE = re.compile(r"Dest Router=(\d+)")
SCHED_TYPE_RE = re.compile(r"\btype = (\w+)")
CONSUME_RE = re.compile(r"Router\[(\d+)\] Consuming:(\S+)")
SA_RE = re.compile(
    r"SwitchAllocator at Router (\d+) granted outvc \d+ at outport (\S+) "
    r"to invc \d+ at inport (\S+) to flit"
)


# ----------------------------------------------------------------------------
# Data types -- named so the parsing/assembly code reads by field, not index.
# Field order is (tick, ...) throughout so the natural tuple sort is by time.
# ----------------------------------------------------------------------------

class ProtoRow(NamedTuple):
    """A ProtocolTrace row for the target address."""
    tick: int
    version: int    # controller version; RNF < HNF_VERSION_BASE <= HNF
    event: str      # SLICC transition label (column 4)
    state: str      # state transition (column 5)
    note: str       # trailing text after the [addr, ...] bracket


class SchedRow(NamedTuple):
    """A NetworkInterface 'Scheduling' (flit injection) row."""
    tick: int
    pid: int        # PacketId
    vnet: int
    src_router: int
    dst_router: int
    mtype: str      # message type, e.g. ReadShared / CompData_SD_PD


class Hop(NamedTuple):
    """A per-router event for a followed flit.

    consume -> flit arrives on `inport` (outport is None);
    sa      -> SwitchAllocator grants `inport` -> `outport`.
    """
    tick: int
    kind: str       # 'consume' or 'sa'
    router: int
    inport: str
    outport: Optional[str]


class Event(NamedTuple):
    """An assembled timeline entry; `rank` orders events sharing a tick."""
    tick: int
    rank: int
    desc: str


@dataclass
class Txn:
    """One selected read transaction and its resolved network identity."""
    addr: str                  # cache-line address, e.g. 0x9a80
    rnf: int                   # requesting RNF controller id
    hnf: Optional[int]         # home node id (None if HNF leg untraced)
    start_tick: int            # START event (ticks)
    end_tick: int              # END = completing data beat (ticks)
    hnf_tick: Optional[int]    # request-leg HNF arrival (None if untraced)
    req_type: str              # request message type, e.g. ReadShared
    rnf_router: Optional[int]  # mesh router hosting the RNF
    hnf_router: Optional[int]  # mesh router hosting the HNF
    req_pid: Optional[int]     # PacketId of the request flit
    data_pid: Optional[int]    # PacketId of the completing data beat
    proto: list[ProtoRow]      # pass-1 ProtocolTrace rows (sorted by tick)
    sched: list[SchedRow]      # pass-1 injection rows (sorted by tick)


# ----------------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------------

def open_trace(path: str) -> TextIO:
    """Open a plain or gzip-compressed trace as a latin-1 text stream."""
    if path.endswith(".gz"):
        return gzip.open(path, "rt", encoding="latin-1")
    return open(path, "rt", encoding="latin-1")


def to_clk(ticks: int) -> int | float:
    """Ticks -> clocks (exact int when a CLOCK_PERIOD multiple, else float)."""
    q, r = divmod(ticks, CLOCK_PERIOD)
    return q if r == 0 else ticks / CLOCK_PERIOD


def loc_tag(version: int) -> str:
    """'rnf<v>' or 'hnf<v-16>' for a ProtocolTrace controller version."""
    if version < HNF_VERSION_BASE:
        return "rnf%d" % version
    return "hnf%d" % (version - HNF_VERSION_BASE)


# ----------------------------------------------------------------------------
# Pass 1 -- collect address-filtered rows
# ----------------------------------------------------------------------------

def collect_for_addr(path: str,
                     addr: str) -> tuple[list[ProtoRow], list[SchedRow]]:
    """Collect every ProtocolTrace and injection row mentioning one address."""
    needle = addr + ","          # 'addr,' avoids 0x9a80 matching 0x9a800,
    proto: list[ProtoRow] = []
    sched: list[SchedRow] = []
    with open_trace(path) as fh:
        for line in fh:
            if needle not in line:
                continue
            if SCHED_RE.search(line):
                pid = PKTID_RE.search(line)
                tk = TICK_RE.match(line)
                mt = SCHED_TYPE_RE.search(line)
                if not (pid and tk and mt):
                    continue
                vnet = SCHED_VNET_RE.search(line)
                src = SCHED_SRC_RE.search(line)
                dst = SCHED_DST_RE.search(line)
                sched.append(SchedRow(
                    int(tk.group(1)), int(pid.group(1)),
                    int(vnet.group(1)) if vnet else -1,
                    int(src.group(1)) if src else -1,
                    int(dst.group(1)) if dst else -1,
                    mt.group(1),
                ))
                continue
            m = PROTO_RE.match(line)
            if m:
                proto.append(ProtoRow(
                    int(m.group(1)), int(m.group(2)),
                    m.group(3), m.group(4), m.group(6).strip()))
    return proto, sched


# ----------------------------------------------------------------------------
# Transaction selection
# ----------------------------------------------------------------------------

def select_txn(proto: list[ProtoRow], sched: list[SchedRow], addr: str,
               start_cycle: Optional[int]) -> Txn:
    """Pick one read transaction and resolve its events + network PacketIds."""
    proto.sort()
    sched.sort()

    # START: RNF SendRead* candidates.
    starts = [p for p in proto
              if p.version < HNF_VERSION_BASE and START_RE.match(p.event)]
    if not starts:
        sys.exit("error: no SendRead* event found for addr %s" % addr)
    if start_cycle is None:
        start = starts[0]
    else:
        target = start_cycle * CLOCK_PERIOD
        start = min(starts, key=lambda p: abs(p.tick - target))

    rnf = start.version
    start_tick = start.tick
    # Network request message type = START event without the "Send" prefix.
    req_type = (start.event[4:] if start.event.startswith("Send")
                else start.event)

    # END: DATA_BEATS-th CompData* back at the RNF, after START.
    beats = [p for p in proto if p.version == rnf and DATA_RE.match(p.event)
             and p.tick >= start_tick]
    if len(beats) < DATA_BEATS:
        sys.exit("error: only %d CompData* beats after START for addr %s "
                 "(need %d)" % (len(beats), addr, DATA_BEATS))
    end_tick = beats[DATA_BEATS - 1].tick

    def win(tk: int) -> bool:
        return start_tick <= tk <= end_tick

    # HNF arrival: first HNF Read* in the window.
    hnf_evs = [p for p in proto if p.version >= HNF_VERSION_BASE
               and HNF_RE.match(p.event) and win(p.tick)]
    hnf = hnf_evs[0].version - HNF_VERSION_BASE if hnf_evs else None
    hnf_tick = hnf_evs[0].tick if hnf_evs else None

    # Request PacketId: Scheduling of req_type within the window.
    req = [s for s in sched if s.mtype == req_type and win(s.tick)]
    req_pid = req[0].pid if req else None
    rnf_router = req[0].src_router if req else None
    hnf_router = req[0].dst_router if req else None

    # Data flit: the completing beat = last of DATA_BEATS CompData* injections
    # from the HNF router. Only this single flit is routed hop-by-hop.
    data = sorted(s for s in sched if DATA_RE.match(s.mtype) and win(s.tick)
                  and (hnf_router is None or s.src_router == hnf_router))
    beat_pids = [s.pid for s in data[:DATA_BEATS]]
    data_pid = beat_pids[-1] if beat_pids else None

    return Txn(addr=addr, rnf=rnf, hnf=hnf, start_tick=start_tick,
               end_tick=end_tick, hnf_tick=hnf_tick, req_type=req_type,
               rnf_router=rnf_router, hnf_router=hnf_router,
               req_pid=req_pid, data_pid=data_pid, proto=proto, sched=sched)


# ----------------------------------------------------------------------------
# Pass 2 -- follow PacketIds through the routers
# ----------------------------------------------------------------------------

def collect_hops(path: str, pids: list[int]) -> dict[int, list[Hop]]:
    """Follow each PacketId hop-by-hop, returning {pid: [Hop, ...]}."""
    hops: dict[int, list[Hop]] = {p: [] for p in pids}
    with open_trace(path) as fh:
        for line in fh:
            is_con = "Consuming" in line
            is_sa = "granted outvc" in line
            if not (is_con or is_sa):
                continue
            pm = PKTID_RE.search(line)
            if not pm:
                continue
            pid = int(pm.group(1))
            if pid not in hops:
                continue
            tk = TICK_RE.match(line)
            if not tk:
                continue
            tick = int(tk.group(1))
            if is_con:
                m = CONSUME_RE.search(line)
                if m:
                    hops[pid].append(
                        Hop(tick, "consume", int(m.group(1)),
                            m.group(2), None))
            else:
                m = SA_RE.search(line)
                if m:
                    hops[pid].append(
                        Hop(tick, "sa", int(m.group(1)),
                            m.group(3), m.group(2)))
    for p in hops:
        hops[p].sort()
    return hops


# ----------------------------------------------------------------------------
# Timeline assembly
# ----------------------------------------------------------------------------

# rank values order events that share a tick.
R_PROTO, R_INJECT, R_CONSUME, R_SA = 0, 1, 2, 3


def build_timeline(t: Txn, hops: dict[int, list[Hop]]) -> list[Event]:
    """Merge protocol, injection and per-hop rows into one ranked timeline."""
    ev: list[Event] = []

    # Protocol events for the address inside the window.
    for tick, ver, event, state, note in t.proto:
        if not (t.start_tick <= tick <= t.end_tick):
            continue
        desc = "[%s] %s  %s" % (loc_tag(ver), event, state)
        if note:
            desc += "  (%s)" % note
        ev.append(Event(tick, R_PROTO, desc))

    # Network injections (Scheduling) for our two flits: REQ and the
    # completing data beat.
    role = {t.req_pid: "REQ", t.data_pid: "DATA"}
    for tick, pid, vnet, src, dst, mtype in t.sched:
        if pid not in role:
            continue
        ev.append(Event(tick, R_INJECT,
                        ">> inject %s  PacketId=%d  %s  vnet%d  R%d->R%d"
                        % (role[pid], pid, mtype, vnet, src, dst)))

    # Per-hop consume / SA-grant.
    for pid, hop_list in hops.items():
        tag = role.get(pid, "PKT%d" % pid)
        prev_consume = None
        consume_at: dict[int, int] = {}  # router -> latest consume tick
        for tick, kind, router, inport, outport in hop_list:
            if kind == "consume":
                dhop = ""
                if prev_consume is not None:
                    dhop = "  (+%scy hop)" % to_clk(tick - prev_consume)
                prev_consume = tick
                consume_at[router] = tick
                ev.append(Event(tick, R_CONSUME,
                                "   %s  R%d consume  <%s>%s"
                                % (tag, router, inport, dhop)))
            else:  # sa: arrive->grant minus structural pipeline = queueing
                q = ""
                if router in consume_at:
                    wait = to_clk(tick - consume_at[router])
                    queue = wait - ROUTER_PIPELINE_CY
                    q = ("  (%scy; +%scy queue)" % (wait, queue)
                         if queue > 0 else "  (%scy)" % wait)
                ev.append(Event(tick, R_SA,
                                "   %s  R%d SA-grant  in=%s -> out=%s%s"
                                % (tag, router, inport, outport, q)))
    ev.sort(key=lambda e: (e.tick, e.rank))
    return ev


def collapse(rows: list[Event]) -> list[Event]:
    """Collapse consecutive rows with identical description into one.

    Anchors the run at its FIRST tick; the time spent in the run therefore
    shows up as the DeltaClk of the NEXT distinct event (e.g. a long
    'Resource Stall' run is paid off on the row that follows it)."""
    out: list[Event] = []
    i = 0
    n = len(rows)
    while i < n:
        tick, rank, desc = rows[i]
        j = i + 1
        while j < n and rows[j].desc == desc:
            j += 1
        if j - i > 1:
            last = rows[j - 1].tick
            span = to_clk(last - tick)
            desc = "%s  [x%d, %s..%s clk, spans %scy]" % (
                desc, j - i, to_clk(tick), to_clk(last), span)
        out.append(Event(tick, rank, desc))
        i = j
    return out


def write_trace(t: Txn, ev: list[Event], path: str) -> None:
    """Write the header block and phase-annotated event table to `path`."""
    # Phase boundaries (ticks).
    p2 = t.hnf_tick
    send = [p for p in t.proto if p.event == "SendCompData" and t.hnf_tick
            and p.tick >= t.hnf_tick]
    p3 = send[0].tick if send else (
        min((s.tick for s in t.sched if s.pid == t.data_pid), default=None))

    start_clk = to_clk(t.start_tick)
    hnf_lbl = "hnf%d" % t.hnf if t.hnf is not None else "hnf?"
    with open(path, "w") as fh:
        # Summary header (comment block).
        fh.write("# garnet_trace -- single CHI read transaction\n")
        fh.write("# addr            : %s\n" % t.addr)
        fh.write("# type            : %s\n" % t.req_type)
        fh.write("# RNF             : rnf%d (router %s)\n"
                 % (t.rnf, t.rnf_router if t.rnf_router is not None else "?"))
        fh.write("# HNF             : %s (router %s)\n"
                 % (("hnf%d" % t.hnf) if t.hnf is not None else "?",
                    t.hnf_router if t.hnf_router is not None else "?"))
        fh.write("# START tick/clk  : %d / %s\n" % (t.start_tick, start_clk))
        fh.write("# END   tick/clk  : %d / %s\n"
                 % (t.end_tick, to_clk(t.end_tick)))
        fh.write("# latency         : %s clk\n"
                 % to_clk(t.end_tick - t.start_tick))
        # Per-phase durations (clk): request leg, HNF internal, data leg.
        if p2 is not None:
            fh.write("# phase1 req leg  : %s clk  (START -> HNF arrival)\n"
                     % to_clk(p2 - t.start_tick))
        if p2 is not None and p3 is not None:
            fh.write("# phase2 HNF      : %s clk  (HNF arrival -> SendData)\n"
                     % to_clk(p3 - p2))
        if p3 is not None:
            fh.write("# phase3 data leg : %s clk  (SendData -> END)\n"
                     % to_clk(t.end_tick - p3))
        fh.write("# request PacketId: %s\n" % t.req_pid)
        fh.write("# data PacketId   : %s  (completing beat %d/%d)\n"
                 % (t.data_pid, DATA_BEATS, DATA_BEATS))
        fh.write("#\n")
        fh.write(ROW_FMT % ("Clk", "Tick", "DeltaClk", "TotalClk",
                            "EventDescription") + "\n")

        # Phase headers, drained in tick order as the timeline crosses each
        # boundary (START -> HNF arrival -> data send).
        markers = [(t.start_tick,
                    "Phase 1: request  rnf%d -> %s" % (t.rnf, hnf_lbl))]
        if p2 is not None:
            markers.append((p2, "Phase 2: HNF internal (%s)" % hnf_lbl))
        if p3 is not None:
            markers.append(
                (p3, "Phase 3: data  %s -> rnf%d" % (hnf_lbl, t.rnf)))
        markers.sort()

        mi = 0
        prev_clk = None
        for tick, _rank, desc in ev:
            while mi < len(markers) and tick >= markers[mi][0]:
                fh.write("# ==== %s ====\n" % markers[mi][1])
                mi += 1
            clk = to_clk(tick)
            dclk = "" if prev_clk is None else _fmt(clk - prev_clk)
            total = _fmt(clk - start_clk)
            fh.write(ROW_FMT % (clk, tick, dclk, total, desc) + "\n")
            prev_clk = clk


def _fmt(x: int | float) -> int | float:
    """Format a clock delta as int when whole."""
    return int(x) if isinstance(x, float) and x.is_integer() else x


# ----------------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------------

def main(argv: list[str]) -> None:
    if len(argv) < 2:
        sys.exit(__doc__)
    path = argv[0]
    addr = "0x%x" % int(argv[1], 16)
    start_cycle = int(argv[2]) if len(argv) > 2 else None

    sys.stderr.write("pass 1: scanning %s for addr %s ...\n" % (path, addr))
    proto, sched = collect_for_addr(path, addr)
    if not proto:
        sys.exit("error: no ProtocolTrace rows for addr %s" % addr)

    t = select_txn(proto, sched, addr, start_cycle)
    sys.stderr.write(
        "selected txn: %s %s  rnf%d -> %s  START %s  END %s  (%s clk)\n"
        % (t.req_type, addr, t.rnf,
           ("hnf%d" % t.hnf) if t.hnf is not None else "hnf?",
           to_clk(t.start_tick), to_clk(t.end_tick),
           to_clk(t.end_tick - t.start_tick)))

    pids = [p for p in [t.req_pid, t.data_pid] if p is not None]
    sys.stderr.write("pass 2: following PacketIds %s through routers ...\n"
                     % pids)
    hops = collect_hops(path, pids) if pids else {}

    ev = collapse(build_timeline(t, hops))
    out = "trace_%s_%s.txt" % (t.req_type, addr)
    write_trace(t, ev, out)
    sys.stderr.write("wrote %s  (%d rows)\n" % (out, len(ev)))


if __name__ == "__main__":
    main(sys.argv[1:])
