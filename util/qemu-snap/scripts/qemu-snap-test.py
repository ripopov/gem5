#!/usr/bin/env python3
"""qemu-snap-test.py - end-to-end test harness for gem5 QEMU-snapshot mode.

This harness is generic: it knows how to *restore a snapshot into gem5 and
validate the run*, but it has no per-testcase or per-gem5-mode code.  What to
run is the cross product of three independent axes:

  * testcase   - any workload registered in testcases.py (--test);
  * CPU model  - atomic / timing / o3 / minor (--cpu);
  * memory     - classic, or Ruby/CHI with the simple or Garnet NoC (--mem).

Every (testcase x CPU x memory) combination is a valid run, so any testcase
can be exercised in any gem5 mode.  How a testcase is validated comes from its
testcases.py entry ('terminal' = a console pass-marker; 'interactive' = type a
command and check it runs); the harness itself never mentions a testcase by
name.

Snapshots are expected under snapshots/<testcase>/ (override the root with
--snapshot-root).  Pass --capture to capture any missing snapshot up front by
invoking qemu-snapshot.py.

Examples
--------
  # default: every testcase, timing CPU, classic memory
  qemu-snap-test.py

  # the syscall testcase on O3, both classic and Ruby/CHI+Garnet
  qemu-snap-test.py --test syscall --cpu o3 --mem classic,ruby-garnet

  # full sweep, capturing any missing snapshots first
  qemu-snap-test.py --test all --cpu atomic,timing,o3,minor --mem all --capture
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", "..", ".."))
GEM5 = os.path.join(REPO_ROOT, "build", "RISCV", "gem5.opt")
RESTORE = os.path.join(REPO_ROOT, "configs", "example", "qemu_snap",
                       "restore.py")
SNAPSHOT_PY = os.path.join(SCRIPT_DIR, "qemu-snapshot.py")

sys.path.insert(0, SCRIPT_DIR)
import testcases                                              # noqa: E402

# The memory-system axis: a name -> extra restore.py flags.  Ruby needs a
# timing-class CPU, so 'atomic' is skipped for the ruby-* configs.
MEM_CONFIGS = {
    "classic":     [],
    "ruby-simple": ["--ruby", "--network", "simple"],
    "ruby-garnet": ["--ruby", "--network", "garnet"],
}


def log(msg):
    print("\033[1;32m[qemu-snap-test]\033[0m %s" % msg, flush=True)


# --------------------------------------------------------------------------
# gem5 output helpers
# --------------------------------------------------------------------------
def parse_stats(path):
    """Parse a gem5 stats.txt into {name: float} (first stat dump only)."""
    stats = {}
    if not os.path.exists(path):
        return stats
    with open(path) as f:
        for line in f:
            if line.startswith("---"):        # end of the first dump
                if stats:
                    break
                continue
            line = line.split("#", 1)[0].split()
            if len(line) >= 2:
                try:
                    stats[line[0]] = float(line[1])
                except ValueError:
                    pass
    return stats


def sum_stats(stats, suffix):
    """Sum every stat whose name ends with the given suffix."""
    return sum(v for k, v in stats.items() if k.endswith(suffix))


def read_terminal(outdir):
    """Return the gem5 guest console transcript, or ''."""
    tf = os.path.join(outdir, "system.platform.terminal")
    return open(tf).read() if os.path.exists(tf) else ""


def exit_cause(stdout):
    """Extract gem5's exit reason from its stdout."""
    m = re.search(r"exit @ tick \d+ : (.*)", stdout)
    return m.group(1) if m else ""


def snapshot_harts(snap):
    """Hart count recorded in a snapshot's meta.json (0 if unreadable)."""
    try:
        with open(os.path.join(snap, "meta.json")) as f:
            return int(json.load(f)["num_harts"])
    except (OSError, ValueError, KeyError):
        return 0


def cpu_cycles(stats, num_harts):
    """Per-hart numCycles.  gem5 names a lone CPU `system.cpu` but an SMP
    set `system.cpu0`, `system.cpu1`, ... - handle both."""
    if num_harts == 1 and "system.cpu.numCycles" in stats:
        return [stats["system.cpu.numCycles"]]
    return [stats.get("system.cpu%d.numCycles" % i, 0.0)
            for i in range(num_harts)]


# --------------------------------------------------------------------------
# snapshot management
# --------------------------------------------------------------------------
def ensure_snapshot(tc, snap, do_capture):
    """Make sure snapshot `snap` for testcase `tc` exists.

    Returns True if it is present (or was just captured), False otherwise.
    """
    if os.path.exists(os.path.join(snap, "meta.json")):
        return True
    if not do_capture:
        log("%s: snapshot %s missing (run qemu-snapshot.py, or pass "
            "--capture)" % (tc.name, snap))
        return False
    log("%s: capturing snapshot -> %s" % (tc.name, snap))
    res = subprocess.run([SNAPSHOT_PY, "--test", tc.name, "--out", snap])
    if res.returncode != 0 or not os.path.exists(os.path.join(snap,
                                                              "meta.json")):
        log("%s: snapshot capture FAILED" % tc.name)
        return False
    return True


def restore_cmd(snap, cpu, mem, outdir, timer_gap):
    """Build the gem5 restore.py command for one (snapshot, cpu, mem) run."""
    cmd = [GEM5, "--outdir=" + outdir, RESTORE,
           "--snapshot-dir", snap, "--cpu", cpu]
    if timer_gap:
        cmd += ["--timer-gap", str(timer_gap)]
    cmd += MEM_CONFIGS[mem]
    return cmd


# --------------------------------------------------------------------------
# the two validation strategies (selected by testcase.check)
# --------------------------------------------------------------------------
def run_terminal(tc, snap, cpu, mem, outdir, timeout):
    """Restore, run to m5_exit, and check the console pass-marker.

    Validates, for any testcase whose check is 'terminal':
      - the testcase's pass_marker appears on the gem5 console;
      - gem5 exited via the m5_exit instruction;
      - every hart advanced its cycle counter (the workload really ran on
        all cores, not just hart 0);
      - for a Ruby memory config, the CHI cache hierarchy and DRAM
        controller show real traffic.
    """
    try:
        res = subprocess.run(restore_cmd(snap, cpu, mem, outdir,
                                         tc.timer_gap),
                             capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return False, "TIMEOUT"

    term = read_terminal(outdir)
    cause = exit_cause(res.stdout)
    stats = parse_stats(os.path.join(outdir, "stats.txt"))

    num_harts = snapshot_harts(snap)
    cycles = cpu_cycles(stats, num_harts)
    all_ran = num_harts > 0 and all(c > 0 for c in cycles)

    marker_ok = tc.pass_marker in term
    exit_ok = "m5_exit" in cause

    detail = "cycles/hart=%s exit=%r" % ([int(c) for c in cycles], cause)
    ok = marker_ok and exit_ok and all_ran

    if mem != "classic":
        # The CHI hierarchy and DRAM must have seen genuine traffic.
        cache_acc = sum_stats(stats, "cache.m_demand_accesses")
        cache_miss = sum_stats(stats, "cache.m_demand_misses")
        hnf_reqs = stats.get("system.ruby.hnf0.cntrl.reqIn.m_msg_count", 0.0)
        dram_rd = stats.get("system.mem_ctrls.readReqs", 0.0)
        dram_wr = stats.get("system.mem_ctrls.writeReqs", 0.0)
        stats_ok = (cache_acc > 0 and cache_miss > 0 and hnf_reqs > 0
                    and dram_rd > 0 and dram_wr > 0)
        detail += (" | CHI acc=%d miss=%d HNF=%d DRAM rd=%d wr=%d"
                   % (int(cache_acc), int(cache_miss), int(hnf_reqs),
                      int(dram_rd), int(dram_wr)))
        ok = ok and stats_ok

    return ok, detail


def run_interactive(tc, snap, cpu, mem, outdir, timeout):
    """Restore an idle-shell snapshot, type a command and check it executed.

    The command's *output* does not appear in the typed text, so finding it
    proves the restored shell woke on the UART interrupt, read the line and
    actually ran it - not just tty-echoed it.
    """
    command = "echo OUT$((7*9))END\n"
    marker = b"OUT63END"

    proc = subprocess.Popen(
        [GEM5, "--listener-mode=on"] + restore_cmd(snap, cpu, mem, outdir,
                                                   tc.timer_gap)[1:],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        bufsize=1)

    state = {"port": None, "running": False}

    def reader():
        for line in proc.stdout:
            m = re.search(r"Listening for connections on port (\d+)", line)
            if m:
                state["port"] = int(m.group(1))
            if "starting simulation" in line:
                state["running"] = True

    threading.Thread(target=reader, daemon=True).start()

    try:
        deadline = time.time() + timeout
        while time.time() < deadline and not (state["port"]
                                              and state["running"]):
            if proc.poll() is not None:
                break
            time.sleep(0.2)
        if not state["port"]:
            return False, "gem5 terminal never started listening"

        time.sleep(3.0)                       # let the restored guest settle
        sock = socket.create_connection(("127.0.0.1", state["port"]),
                                        timeout=10)
        sock.settimeout(1.0)
        time.sleep(1.0)
        sock.sendall(command.encode())

        rx = b""
        rxdeadline = time.time() + min(60.0, timeout)
        while time.time() < rxdeadline and marker not in rx:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                break
            rx += chunk
        sock.close()
        ok = marker in rx
        return ok, "terminal echoed %r" % rx.decode(errors="replace").strip()
    finally:
        proc.kill()


CHECKS = {"terminal": run_terminal, "interactive": run_interactive}


# --------------------------------------------------------------------------
def run_one(tc, snap, cpu, mem, timeout):
    """Run and validate a single (testcase, cpu, mem) combination."""
    label = "%s/%s/%s" % (tc.name, cpu, mem)
    if mem != "classic" and cpu == "atomic":
        log("%s: SKIP (Ruby needs a timing-class CPU)" % label)
        return None
    outdir = os.path.join(REPO_ROOT, "m5out",
                          "qcputest_%s_%s_%s" % (tc.name, cpu, mem))
    log("%s: restoring %s" % (label, snap))
    ok, detail = CHECKS[tc.check](tc, snap, cpu, mem, outdir, timeout)
    log("%s: %s -> %s" % (label, detail, "PASS" if ok else "FAIL"))
    return ok


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--test", default="all",
                    help="comma-separated testcase names, or 'all' (%s)"
                         % ", ".join(testcases.NAMES))
    ap.add_argument("--cpu", default="timing",
                    help="comma-separated CPU models "
                         "(atomic,timing,o3,minor)")
    ap.add_argument("--mem", default="classic",
                    help="comma-separated memory configs, or 'all' (%s)"
                         % ", ".join(MEM_CONFIGS))
    ap.add_argument("--snapshot-root",
                    default=os.path.join(REPO_ROOT, "snapshots"),
                    help="directory holding snapshots/<testcase>/")
    ap.add_argument("--capture", action="store_true",
                    help="capture any missing snapshot via qemu-snapshot.py")
    ap.add_argument("--timeout", type=float, default=900.0)
    ap.add_argument("--list", action="store_true",
                    help="list the registered testcases and exit")
    args = ap.parse_args()

    if args.list:
        testcases._main(["list"])
        return

    if not os.path.exists(GEM5):
        sys.exit("missing %s - build gem5 first" % GEM5)

    tests = (testcases.NAMES if args.test == "all"
             else [t.strip() for t in args.test.split(",")])
    cpus = [c.strip() for c in args.cpu.split(",")]
    mems = (list(MEM_CONFIGS) if args.mem == "all"
            else [m.strip() for m in args.mem.split(",")])
    for m in mems:
        if m not in MEM_CONFIGS:
            sys.exit("unknown --mem %r (known: %s)"
                     % (m, ", ".join(MEM_CONFIGS)))

    results = []
    for name in tests:
        tc = testcases.get(name)
        snap = os.path.join(args.snapshot_root, tc.name)
        if not ensure_snapshot(tc, snap, args.capture):
            for cpu in cpus:
                for mem in mems:
                    results.append(("%s/%s/%s" % (tc.name, cpu, mem),
                                    "NOSNAP"))
            continue
        for cpu in cpus:
            for mem in mems:
                ok = run_one(tc, snap, cpu, mem, args.timeout)
                label = "%s/%s/%s" % (tc.name, cpu, mem)
                if ok is None:
                    results.append((label, "SKIP"))
                else:
                    results.append((label, "PASS" if ok else "FAIL"))

    print()
    log("==== results ====")
    allok = True
    for name, status in results:
        print("    %-26s %s" % (name, status))
        if status in ("FAIL", "NOSNAP"):
            allok = False
    sys.exit(0 if allok else 1)


if __name__ == "__main__":
    main()
