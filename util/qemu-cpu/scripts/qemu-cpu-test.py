#!/usr/bin/env python3
"""qemu-cpu-test.py - end-to-end tests for gem5 QEMU-CPU mode.

  bench test       : restore the benchmark snapshot, run it on a detailed
                     CPU, and check it prints the correct checksum on the
                     (interrupt-driven) console.
  interactive test : restore the idle-shell snapshot, connect to gem5's
                     terminal, type a command, and check the restored shell
                     wakes on the UART interrupt and runs it.

Both snapshots must already exist (run qemu-snapshot.py first):
    snapshots/bench   (qemu-snapshot.py --mode bench)
    snapshots/shell   (qemu-snapshot.py --mode shell)
"""
import argparse
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
# Test 2: idle-shell restore -> interactive
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
    ap.add_argument("--test", choices=["bench", "interactive", "all"],
                    default="all")
    ap.add_argument("--cpu", default="atomic",
                    help="CPU model(s), comma-separated")
    ap.add_argument("--bench-snap",
                    default=os.path.join(REPO_ROOT, "snapshots", "bench"),
                    help="benchmark snapshot directory")
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
        if args.test in ("interactive", "all"):
            results.append(("interactive/%s" % cpu,
                             test_interactive(cpu, args.timeout,
                                              args.shell_snap)))

    print()
    log("==== results ====")
    allok = True
    for name, ok in results:
        print("    %-22s %s" % (name, "PASS" if ok else "FAIL"))
        allok = allok and ok
    sys.exit(0 if allok else 1)


if __name__ == "__main__":
    main()
