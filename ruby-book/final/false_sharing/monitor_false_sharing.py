#!/usr/bin/env python3
"""
monitor_false_sharing.py -- Low-noise progress monitor for Stage 3c runs.

Usage:
    python3 monitor_false_sharing.py <pid> <console-log>
"""

from __future__ import annotations

import os
import re
import sys
import time

HEARTBEAT_SECS = 20.0
POLL_SECS = 2.0

PHASE_RE = re.compile(r"^RBOOK_FALSE_PHASE\s+(.+?)\s*$")
PROGRESS_RE = re.compile(r"^RBOOK_FALSE_PROGRESS\s+(\d+)/(\d+)\s*$")


def format_duration(seconds: float) -> str:
    total = max(0, int(round(seconds)))
    minutes, sec = divmod(total, 60)
    hours, minutes = divmod(minutes, 60)
    if hours:
        return f"{hours:d}:{minutes:02d}:{sec:02d}"
    return f"{minutes:02d}:{sec:02d}"


def process_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def read_new_lines(path: str, offset: int) -> tuple[list[str], int]:
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            fh.seek(offset)
            lines = fh.readlines()
            return lines, fh.tell()
    except FileNotFoundError:
        return [], offset


def main() -> None:
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <pid> <console-log>", file=sys.stderr)
        sys.exit(1)

    pid = int(sys.argv[1], 0)
    console_path = sys.argv[2]
    run_started = time.monotonic()
    measured_started = None
    current_phase = "BOOTING"
    last_heartbeat = 0.0
    offset = 0
    print(
        f"[false_sharing] monitoring {console_path} for pid {pid}",
        flush=True,
    )

    while True:
        lines, offset = read_new_lines(console_path, offset)
        now = time.monotonic()

        for raw in lines:
            line = raw.rstrip("\n")
            phase_match = PHASE_RE.match(line)
            if phase_match:
                current_phase = phase_match.group(1)
                if current_phase == "MEASURED_START":
                    measured_started = now
                print(
                    f"[false_sharing] phase={current_phase} "
                    f"elapsed={format_duration(now - run_started)}",
                    flush=True,
                )
                continue

            progress_match = PROGRESS_RE.match(line)
            if progress_match:
                completed = int(progress_match.group(1))
                total = int(progress_match.group(2))
                if measured_started is None:
                    measured_started = now
                elapsed = now - measured_started
                remaining = 0.0
                if completed > 0:
                    remaining = elapsed * (total - completed) / completed
                print(
                    f"[false_sharing] measured {completed}/{total} "
                    f"({100.0 * completed / total:.1f}%) "
                    f"elapsed={format_duration(elapsed)} "
                    f"eta={format_duration(remaining)}",
                    flush=True,
                )
        alive = process_alive(pid)
        if not alive:
            lines, offset = read_new_lines(console_path, offset)
            for raw in lines:
                line = raw.rstrip("\n")
                phase_match = PHASE_RE.match(line)
                if phase_match:
                    current_phase = phase_match.group(1)
            print(
                f"[false_sharing] process exited, final phase={current_phase}, "
                f"total elapsed={format_duration(now - run_started)}",
                flush=True,
            )
            return

        if now - last_heartbeat >= HEARTBEAT_SECS:
            log_size = 0
            try:
                log_size = os.path.getsize(console_path)
            except FileNotFoundError:
                pass
            print(
                f"[false_sharing] heartbeat phase={current_phase} "
                f"elapsed={format_duration(now - run_started)} "
                f"log={log_size / 1024.0:.1f} KiB",
                flush=True,
            )
            last_heartbeat = now

        time.sleep(POLL_SECS)


if __name__ == "__main__":
    main()
