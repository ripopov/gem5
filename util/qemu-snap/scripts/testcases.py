#!/usr/bin/env python3
"""testcases.py - the qemu-snap testcase registry (single source of truth).

A *testcase* is a guest workload plus a description of how to snapshot it and
how to tell, from a restored gem5 run, whether it passed.  It is deliberately
decoupled from two orthogonal things:

  * the *infrastructure* - building the image, capturing a snapshot, driving
    gem5 - which is generic and never mentions an individual testcase;
  * the *gem5 mode* - CPU model (atomic/timing/o3/minor) and memory system
    (classic or Ruby/CHI, simple or Garnet network) - which is chosen at run
    time, so any testcase can be run in any mode.

This module is the only place a testcase is named.  It is imported by
qemu-snapshot.py and qemu-snap-test.py, and invoked as a CLI helper by the
(bash) build-image.sh.

Adding a testcase
-----------------
  1. drop a self-contained C file in util/qemu-snap/bench/;
  2. add one TestCase(...) entry to TESTCASES below.

That is all.  build-image.sh compiles every registered source into the
initramfs as /bin/<name>, the generic /init runs /bin/<name> when the kernel
command line says qemusnap.test=<name>, qemu-snapshot.py captures it, and
qemu-snap-test.py validates it - none of those scripts needs to be touched.

A testcase C file must provide a non-inlined snapshot_barrier() function (the
race-free capture point) unless it uses 'marker' capture; see bench/bench.c.

CLI (used by build-image.sh):
  testcases.py sources   one "name<TAB>repo-relative-source<TAB>cflags" line
                         per testcase that has a C source.
  testcases.py names     space-separated list of every testcase name.
  testcases.py list      human-readable summary.
"""
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", "..", ".."))

# C testcase sources live here (repo-relative).
BENCH_DIR = os.path.join("util", "qemu-snap", "bench")


class TestCase:
    """One guest workload and how to snapshot / validate it.

    Fields
    ------
    name        identifier; the initramfs binary is /bin/<name> and the
                default snapshot directory is snapshots/<name>.
    summary     one-line human description.
    source      C source basename in bench/, or None for a testcase that
                needs no binary (the bare-shell 'shell' testcase).
    cflags      extra compiler flags for build-image.sh (e.g. -pthread).
    smp         default hart count to capture the snapshot with.

    capture     how qemu-snapshot.py halts QEMU to take the snapshot:
                  'breakpoint' - a gdb breakpoint on `barrier` (race-free,
                                 the workload halts at exactly that insn);
                  'marker'     - wait for `marker` on the serial console.
    barrier     (breakpoint capture) symbol to breakpoint on.
    marker      (marker capture) serial-console string to snapshot on.
    settle      (marker capture) delay after the marker before the QMP stop.

    check       how qemu-snap-test.py validates a restored gem5 run:
                  'terminal'    - `pass_marker` must appear on the gem5
                                  console and every hart must have advanced;
                  'interactive' - connect to the gem5 terminal, type a
                                  command and check it is executed.
    pass_marker (terminal check) console substring that means success.
    timer_gap   default restore.py --timer-gap for this testcase (0 = none);
                clamps restored timers so an idle guest is not fast-forwarded.
    """

    def __init__(self, name, summary, *, source=None, cflags=(), smp=1,
                 capture="breakpoint", barrier="snapshot_barrier",
                 marker=None, settle=2.0, check="terminal",
                 pass_marker=None, timer_gap=0):
        self.name = name
        self.summary = summary
        self.source = source
        self.cflags = list(cflags)
        self.smp = smp
        self.capture = capture
        self.barrier = barrier
        self.marker = marker
        self.settle = settle
        self.check = check
        self.pass_marker = pass_marker
        self.timer_gap = timer_gap

    @property
    def source_path(self):
        """Repo-relative path of the C source, or None."""
        return os.path.join(BENCH_DIR, self.source) if self.source else None


# --------------------------------------------------------------------------
# The registry.  Order is the order tests run / are listed in.
# --------------------------------------------------------------------------
TESTCASES = [
    TestCase(
        "bench", "single-core CPU-bound matrix multiply",
        source="bench.c", smp=1,
        capture="breakpoint", check="terminal",
        pass_marker="BENCH-DONE ok"),

    TestCase(
        "philo", "multicore dining philosophers (SMP pthreads)",
        source="philo.c", cflags=["-pthread"], smp=4,
        capture="breakpoint", check="terminal",
        pass_marker="PHILO-DONE ok", timer_gap=100000),

    TestCase(
        "syscall", "Linux syscall / kernel exerciser",
        source="syscall.c", smp=1,
        capture="breakpoint", check="terminal",
        pass_marker="SYSCALL-DONE ok", timer_gap=100000),

    TestCase(
        "shell", "idle interactive shell",
        source=None, smp=1,
        capture="marker", marker="QEMU-SNAP-MODE-SHELL-READY",
        check="interactive", timer_gap=200000),
]

BY_NAME = {t.name: t for t in TESTCASES}
NAMES = [t.name for t in TESTCASES]


def get(name):
    """Look up a testcase by name, or exit with a helpful message."""
    try:
        return BY_NAME[name]
    except KeyError:
        raise SystemExit("unknown testcase %r (known: %s)"
                         % (name, ", ".join(NAMES)))


# --------------------------------------------------------------------------
# CLI - consumed by build-image.sh
# --------------------------------------------------------------------------
def _main(argv):
    cmd = argv[0] if argv else "list"
    if cmd == "sources":
        for t in TESTCASES:
            if t.source:
                print("%s\t%s\t%s"
                      % (t.name, t.source_path, " ".join(t.cflags)))
    elif cmd == "names":
        print(" ".join(NAMES))
    elif cmd == "list":
        for t in TESTCASES:
            if t.capture == "breakpoint":
                how = "breakpoint @%s()" % t.barrier
            else:
                how = "marker %r" % t.marker
            print("  %-9s smp=%-2d %-12s %-24s %s"
                  % (t.name, t.smp, t.check, how, t.summary))
    else:
        raise SystemExit("usage: testcases.py {list|sources|names}")


if __name__ == "__main__":
    _main(sys.argv[1:])
