#!/usr/bin/env python3
"""qemu-cpu-test.py - end-to-end tests for gem5 QEMU-CPU mode.

  bench test       : restore the matrix-benchmark snapshot, run it on a
                     detailed CPU, and check it prints the correct checksum
                     on the (interrupt-driven) console.
  philo test       : restore the multicore dining-philosophers snapshot, run
                     it, check the deterministic result on the console and
                     confirm from stats.txt that every hart executed.
  ruby test        : restore the multicore dining-philosophers snapshot into
                     the Ruby CHI coherent memory subsystem (O3 CPU, CustomMesh
                     NoC), once with the simple network and once with Garnet;
                     check the deterministic result and that the CHI / DRAM
                     statistics are populated and reasonable.
  interactive test : restore the idle-shell snapshot, connect to gem5's
                     terminal, type a command, and check the restored shell
                     wakes on the UART interrupt and runs it.

The snapshots must already exist (run qemu-snapshot.py first):
    snapshots/bench   (qemu-snapshot.py --mode bench)
    snapshots/philo   (qemu-snapshot.py --mode philo --smp 4)
    snapshots/shell   (qemu-snapshot.py --mode shell)
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
RESTORE = os.path.join(REPO_ROOT, "configs", "example", "qemu_cpu",
                       "restore.py")


def log(msg):
    print("\033[1;32m[qemu-cpu-test]\033[0m %s" % msg, flush=True)


# --------------------------------------------------------------------------
# Test 1: benchmark restore -> console output
# --------------------------------------------------------------------------
def test_bench(cpu, timeout, snap):
    outdir = os.path.join(REPO_ROOT, "m5out",
                          "test_bench_%s_%s" % (os.path.basename(snap), cpu))
    log("bench/%s: restoring %s" % (cpu, snap))
    res = subprocess.run(
        [GEM5, "--outdir=" + outdir, RESTORE,
         "--snapshot-dir", snap, "--cpu", cpu],
        capture_output=True, text=True, timeout=timeout)
    term = ""
    tf = os.path.join(outdir, "system.platform.terminal")
    if os.path.exists(tf):
        term = open(tf).read()
    cause = ""
    m = re.search(r"exit @ tick \d+ : (.*)", res.stdout)
    if m:
        cause = m.group(1)
    ok = "BENCH-DONE ok" in term and "m5_exit" in cause
    log("bench/%s: exit=%r terminal=%r -> %s"
        % (cpu, cause, term.strip(), "PASS" if ok else "FAIL"))
    return ok


# --------------------------------------------------------------------------
# Test 2: multicore dining-philosophers restore -> console + per-hart stats
# --------------------------------------------------------------------------
def parse_stats(path):
    """Parse a gem5 stats.txt into {name: float} (first stat dump)."""
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


def test_philo(cpu, timeout, snap):
    outdir = os.path.join(REPO_ROOT, "m5out",
                          "test_philo_%s_%s" % (os.path.basename(snap), cpu))
    try:
        with open(os.path.join(snap, "meta.json")) as f:
            num_harts = json.load(f)["num_harts"]
    except OSError:
        log("philo/%s: missing snapshot %s" % (cpu, snap))
        return False
    log("philo/%s: restoring %s (%d harts)" % (cpu, snap, num_harts))
    res = subprocess.run(
        [GEM5, "--outdir=" + outdir, RESTORE,
         "--snapshot-dir", snap, "--cpu", cpu, "--timer-gap", "100000"],
        capture_output=True, text=True, timeout=timeout)

    term = ""
    tf = os.path.join(outdir, "system.platform.terminal")
    if os.path.exists(tf):
        term = open(tf).read()
    cause = ""
    m = re.search(r"exit @ tick \d+ : (.*)", res.stdout)
    if m:
        cause = m.group(1)

    # Per-hart evidence: every CPU must have advanced its cycle counter,
    # which proves the SMP scheduler actually ran threads on every hart.
    stats = parse_stats(os.path.join(outdir, "stats.txt"))
    cycles = [stats.get("system.cpu%d.numCycles" % i, 0.0)
              for i in range(num_harts)]
    all_ran = all(c > 0 for c in cycles)

    result_ok = "PHILO-DONE ok" in term
    ok = result_ok and "m5_exit" in cause and all_ran
    summary = re.search(r"PHILO-DONE [^\n]*", term)
    log("philo/%s: %s | cycles/hart=%s | exit=%r -> %s"
        % (cpu, summary.group(0) if summary else "(no PHILO-DONE line)",
           [int(c) for c in cycles], cause, "PASS" if ok else "FAIL"))
    return ok


# --------------------------------------------------------------------------
# Test 3: multicore philo restore into a Ruby memory subsystem
# --------------------------------------------------------------------------
def _sum_stats(stats, suffix):
    """Sum every stat whose name ends with the given suffix."""
    return sum(v for k, v in stats.items() if k.endswith(suffix))


def test_ruby(timeout, snap, network):
    """Restore the philo snapshot into Ruby/CHI (O3 CPU, CustomMesh NoC) on
    the given network and validate the result plus the CHI/DRAM statistics."""
    cpu = "o3"
    outdir = os.path.join(REPO_ROOT, "m5out",
                          "test_ruby_%s_%s" % (network,
                                               os.path.basename(snap)))
    try:
        with open(os.path.join(snap, "meta.json")) as f:
            num_harts = json.load(f)["num_harts"]
    except OSError:
        log("ruby-%s: missing snapshot %s" % (network, snap))
        return False
    log("ruby-%s: restoring %s (%d harts, O3 + CHI, CustomMesh NoC)"
        % (network, snap, num_harts))
    res = subprocess.run(
        [GEM5, "--outdir=" + outdir, RESTORE,
         "--snapshot-dir", snap, "--cpu", cpu, "--timer-gap", "100000",
         "--ruby", "--network", network],
        capture_output=True, text=True, timeout=timeout)

    term = ""
    tf = os.path.join(outdir, "system.platform.terminal")
    if os.path.exists(tf):
        term = open(tf).read()
    cause = ""
    m = re.search(r"exit @ tick \d+ : (.*)", res.stdout)
    if m:
        cause = m.group(1)

    stats = parse_stats(os.path.join(outdir, "stats.txt"))
    # Every hart's CPU must have advanced -- a true multicore restore.
    cycles = [stats.get("system.cpu%d.numCycles" % i, 0.0)
              for i in range(num_harts)]
    all_ran = all(c > 0 for c in cycles)
    # The CHI cache hierarchy and DRAM controller must show real traffic:
    #   - the L1/L2/L3 caches saw demand accesses, and some missed (so the
    #     NoC and home nodes were genuinely exercised),
    #   - the L3 home node received requests over the NoC,
    #   - the DRAM controller served reads and writes.
    cache_acc = _sum_stats(stats, "cache.m_demand_accesses")
    cache_miss = _sum_stats(stats, "cache.m_demand_misses")
    hnf_reqs = stats.get("system.ruby.hnf0.cntrl.reqIn.m_msg_count", 0.0)
    dram_rd = stats.get("system.mem_ctrls.readReqs", 0.0)
    dram_wr = stats.get("system.mem_ctrls.writeReqs", 0.0)
    stats_ok = (cache_acc > 0 and cache_miss > 0 and hnf_reqs > 0
                and dram_rd > 0 and dram_wr > 0)

    result_ok = "PHILO-DONE ok" in term
    ok = result_ok and "m5_exit" in cause and all_ran and stats_ok
    summary = re.search(r"PHILO-DONE [^\n]*", term)
    log("ruby-%s: %s | cycles/hart=%s" % (network,
        summary.group(0) if summary else "(no PHILO-DONE line)",
        [int(c) for c in cycles]))
    log("ruby-%s: CHI cache acc=%d miss=%d | HNF reqs=%d | DRAM rd=%d wr=%d "
        "-> %s" % (network, int(cache_acc), int(cache_miss), int(hnf_reqs),
                   int(dram_rd), int(dram_wr), "PASS" if ok else "FAIL"))
    return ok


# --------------------------------------------------------------------------
# Test 4: idle-shell restore -> interactive
# --------------------------------------------------------------------------
def test_interactive(cpu, timeout, snap):
    outdir = os.path.join(REPO_ROOT, "m5out",
                          "test_shell_%s_%s" % (os.path.basename(snap), cpu))
    # The command's *output* (OUT63END) does not appear in the typed text,
    # so finding it proves the restored shell woke on the UART interrupt,
    # read the line and actually executed it - not just tty-echoed it.
    command = "echo OUT$((7*9))END\n"
    marker = "OUT63END"
    log("interactive/%s: restoring %s" % (cpu, snap))

    proc = subprocess.Popen(
        [GEM5, "--listener-mode=on", "--outdir=" + outdir, RESTORE,
         "--snapshot-dir", snap, "--cpu", cpu, "--timer-gap", "200000"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        bufsize=1)

    state = {"port": None, "running": False}
    lines = []

    def reader():
        for line in proc.stdout:
            lines.append(line)
            m = re.search(r"Listening for connections on port (\d+)", line)
            if m:
                state["port"] = int(m.group(1))
            if "starting simulation" in line:
                state["running"] = True

    threading.Thread(target=reader, daemon=True).start()

    ok = False
    try:
        deadline = time.time() + timeout
        while time.time() < deadline and not (state["port"]
                                              and state["running"]):
            if proc.poll() is not None:
                break
            time.sleep(0.2)
        if not state["port"]:
            log("interactive/%s: gem5 terminal never started listening"
                % cpu)
            return False
        log("interactive/%s: gem5 terminal on port %d; restore running"
            % (cpu, state["port"]))

        # Let the restored guest settle (first timer tick etc.).
        time.sleep(3.0)

        sock = socket.create_connection(("127.0.0.1", state["port"]),
                                        timeout=10)
        sock.settimeout(1.0)
        time.sleep(1.0)
        log("interactive/%s: typing %r" % (cpu, command.strip()))
        sock.sendall(command.encode())

        rx = b""
        rxdeadline = time.time() + min(60.0, timeout)
        while time.time() < rxdeadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                if marker.encode() in rx:
                    break
                continue
            if not chunk:
                break
            rx += chunk
            if marker.encode() in rx:
                break
        sock.close()
        ok = marker.encode() in rx
        log("interactive/%s: terminal echoed %r -> %s"
            % (cpu, rx.decode(errors="replace").strip(),
               "PASS" if ok else "FAIL"))
    finally:
        proc.kill()
    return ok


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--test",
                    choices=["bench", "philo", "ruby", "interactive", "all"],
                    default="all")
    ap.add_argument("--cpu", default="atomic",
                    help="CPU model(s), comma-separated")
    ap.add_argument("--bench-snap",
                    default=os.path.join(REPO_ROOT, "snapshots", "bench"),
                    help="matrix-benchmark snapshot directory")
    ap.add_argument("--philo-snap",
                    default=os.path.join(REPO_ROOT, "snapshots", "philo"),
                    help="multicore dining-philosophers snapshot directory")
    ap.add_argument("--shell-snap",
                    default=os.path.join(REPO_ROOT, "snapshots", "shell"),
                    help="idle-shell snapshot directory")
    ap.add_argument("--timeout", type=float, default=900.0)
    args = ap.parse_args()

    if not os.path.exists(GEM5):
        sys.exit("missing %s - build gem5 first" % GEM5)

    results = []
    for cpu in args.cpu.split(","):
        cpu = cpu.strip()
        if args.test in ("bench", "all"):
            results.append(("bench/%s" % cpu,
                             test_bench(cpu, args.timeout, args.bench_snap)))
        if args.test in ("philo", "all"):
            results.append(("philo/%s" % cpu,
                             test_philo(cpu, args.timeout, args.philo_snap)))
        if args.test in ("interactive", "all"):
            results.append(("interactive/%s" % cpu,
                             test_interactive(cpu, args.timeout,
                                              args.shell_snap)))

    # The Ruby tests are not parameterised by --cpu: they always restore the
    # multicore philo snapshot into an O3 CPU (per the validated scenario),
    # once per interconnect.
    if args.test in ("ruby", "all"):
        for network in ("simple", "garnet"):
            results.append(("ruby/%s" % network,
                             test_ruby(args.timeout, args.philo_snap,
                                       network)))

    print()
    log("==== results ====")
    allok = True
    for name, ok in results:
        print("    %-22s %s" % (name, "PASS" if ok else "FAIL"))
        allok = allok and ok
    sys.exit(0 if allok else 1)


if __name__ == "__main__":
    main()
