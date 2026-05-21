#!/usr/bin/env python3
"""qemu-snapshot.py - boot the minimal RISC-V image under QEMU and capture a
full machine snapshot for gem5 QEMU-CPU mode (pipeline stage 2 of 3).

Three capture modes are supported:

  * --mode bench  : a gdb breakpoint on the matrix benchmark's
                    snapshot_barrier() function -- QEMU halts at *exactly*
                    that instruction, so there is no capture-window race.
  * --mode philo  : the same race-free breakpoint barrier, on the multicore
                    dining-philosophers benchmark (use with --smp N>1).
  * --mode shell  : wait for a marker on the serial console, then QMP-stop --
                    used to snapshot the idle interactive shell.

The snapshot directory contains:

    ram.bin            raw guest DRAM image
    clint.bin          CLINT MMIO dump  (mtime / mtimecmp)
    plic.bin           PLIC  MMIO dump  (priority / enable / threshold)
    uart.bin           UART 8250 register dump
    regs.hart<N>.txt   every GPR + FP reg + CSR of hart N (via the gdbstub)
    virt.dtb           the QEMU 'virt' device tree
    serial.log         console transcript
    meta.json          addresses / sizes / hart list, incl. the DTB-derived
                       platform description gem5's restore.py consumes

QEMU is driven through three control planes: a UNIX-socket serial console,
the QMP monitor (pause + dump physical memory) and the gdbstub (breakpoint
barrier + architectural register/CSR read).
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

# Fallback addresses if the DTB cannot be parsed (standard QEMU 'virt').
DEFAULT_RAM_BASE = 0x80000000
DEFAULT_CLINT, CLINT_SIZE = 0x02000000, 0x10000
DEFAULT_PLIC, PLIC_SIZE = 0x0C000000, 0x600000
DEFAULT_UART, UART_SIZE = 0x10000000, 0x100

# Keep in sync with qemu-common.sh:QEMU_CPU
DEFAULT_CPU = ("rv64,v=false,h=false,sstc=false,zicond=false,zacas=false,"
               "zawrs=false,zbc=false,zbkb=false,zfa=false,zfh=false,"
               "zfhmin=false,svadu=false,sv57=false,sv48=false")


def log(msg):
    print("\033[1;35m[qemu-snapshot]\033[0m %s" % msg, flush=True)


# --------------------------------------------------------------------------
# QMP client
# --------------------------------------------------------------------------
class QMP:
    """Minimal QEMU Machine Protocol client over a UNIX socket."""

    def __init__(self, path, timeout=30.0):
        deadline = time.time() + timeout
        self.sock = None
        while time.time() < deadline:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                self.sock = s
                break
            except OSError:
                time.sleep(0.1)
        if self.sock is None:
            raise RuntimeError("could not connect to QMP socket %s" % path)
        self.f = self.sock.makefile("rwb")
        self._recv()                       # server greeting
        self.execute("qmp_capabilities")

    def _recv(self):
        while True:
            line = self.f.readline()
            if not line:
                raise RuntimeError("QMP connection closed")
            msg = json.loads(line)
            if "event" in msg:             # ignore async events
                continue
            return msg

    def execute(self, cmd, **args):
        req = {"execute": cmd}
        if args:
            req["arguments"] = args
        self.f.write((json.dumps(req) + "\n").encode())
        self.f.flush()
        reply = self._recv()
        if "error" in reply:
            raise RuntimeError("QMP %s failed: %s" % (cmd, reply["error"]))
        return reply.get("return")

    def pmemsave(self, addr, size, filename):
        self.execute("pmemsave", val=addr, size=size, filename=filename)


# --------------------------------------------------------------------------
# gdb driver - drives gdb-multiarch over pipes, synced with echo sentinels
# --------------------------------------------------------------------------
class GdbDriver:
    """Drives a long-lived gdb-multiarch session connected to QEMU's gdbstub.

    gdb stays attached for the whole capture so the VM remains halted at the
    breakpoint while QMP dumps physical memory.  Commands are synchronised by
    appending an `echo <sentinel>` and reading stdout until it appears -- this
    copes with `continue` blocking until the breakpoint is hit.
    """

    def __init__(self, port):
        self.proc = subprocess.Popen(
            ["gdb-multiarch", "-q", "-nx"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        self._buf = []
        self._lock = threading.Lock()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        self._n = 0
        self._run("set pagination off")
        self._run("set confirm off")
        self._run("set osabi none")
        if self._run("target remote :%d" % port, timeout=30) is None:
            raise RuntimeError("gdb could not attach to :%d" % port)

    def _read_loop(self):
        for line in self.proc.stdout:
            with self._lock:
                self._buf.append(line)

    def _drain(self):
        with self._lock:
            out = "".join(self._buf)
            self._buf.clear()
            return out

    def send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()

    def _run(self, cmd, timeout=60.0):
        """Run a gdb command, return all output it produced (or None on
        timeout)."""
        self._n += 1
        sentinel = "@@SYNC%d@@" % self._n
        self.send(cmd)
        self.send("echo " + sentinel + "\\n")
        deadline = time.time() + timeout
        acc = ""
        while time.time() < deadline:
            acc += self._drain()
            if sentinel in acc:
                return acc.split(sentinel)[0]
            if self.proc.poll() is not None:
                raise RuntimeError("gdb exited unexpectedly:\n" + acc)
            time.sleep(0.05)
        return None

    def set_breakpoint(self, addr):
        self._run("break *0x%x" % addr)

    def continue_to_breakpoint(self, timeout):
        out = self._run("continue", timeout=timeout)
        if out is None:
            raise RuntimeError("timed out waiting for the snapshot barrier")
        if "Breakpoint" not in out and "received signal" not in out:
            raise RuntimeError("unexpected gdb stop:\n" + out)
        return out

    def delete_breakpoints(self):
        self._run("delete")

    def dump_registers(self):
        out = self._run("source " + os.path.join(SCRIPT_DIR,
                                                  "gdb-dump-regs.py"))
        if out is None or "REGDUMP-OK" not in out:
            raise RuntimeError("register dump failed:\n%s" % out)
        return out

    def quit(self):
        try:
            self.send("detach")
            self.send("quit")
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------
def connect_unix(path, timeout=30.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(path)
            return s
        except OSError:
            time.sleep(0.1)
    raise RuntimeError("could not connect to socket %s" % path)


def serial_drainer(sock, logpath, stop_evt, marker=None, found_evt=None):
    """Background thread: copy the serial console to a log file, and (if a
    marker is given) set found_evt when it appears."""
    sock.settimeout(0.5)
    buf = bytearray()
    with open(logpath, "wb") as logf:
        while not stop_evt.is_set():
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            logf.write(chunk)
            logf.flush()
            if marker is not None and found_evt is not None:
                buf += chunk
                if marker in buf:
                    found_evt.set()


def resolve_symbol(elf, symbol):
    """Return the address of `symbol` in `elf` via riscv64 nm."""
    for nm in ("riscv64-linux-gnu-nm", "nm"):
        try:
            out = subprocess.check_output([nm, elf], text=True)
        except (OSError, subprocess.CalledProcessError):
            continue
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2] == symbol:
                return int(parts[0], 16)
    raise RuntimeError("symbol %r not found in %s" % (symbol, elf))


def parse_regs(text):
    """Parse `REG <hart> <name> 0x<val>` lines into {hart: {name: val}}."""
    harts = {}
    for line in text.splitlines():
        p = line.split()
        if len(p) == 4 and p[0] == "REG":
            try:
                harts.setdefault(int(p[1]), {})[p[2]] = int(p[3], 0)
            except ValueError:
                pass
    return harts


# --------------------------------------------------------------------------
# Device tree: dump from QEMU and parse the platform layout
# --------------------------------------------------------------------------
def dump_and_parse_dtb(qemu, cpu, smp, mem_mb, bios, dtb_path):
    """Dump the QEMU 'virt' DTB and extract the platform layout."""
    subprocess.run(
        [qemu, "-machine", "virt,dumpdtb=%s" % dtb_path, "-cpu", cpu,
         "-smp", str(smp), "-m", "%dM" % mem_mb, "-bios", bios,
         "-display", "none"],
        check=True, capture_output=True, text=True)
    dts = subprocess.check_output(["dtc", "-I", "dtb", "-O", "dts", dtb_path],
                                  text=True, stderr=subprocess.DEVNULL)

    def node_addr(pattern, default):
        m = re.search(pattern + r"@([0-9a-fA-F]+)", dts)
        return int(m.group(1), 16) if m else default

    plat = {
        "ram_base": node_addr("memory", DEFAULT_RAM_BASE),
        "clint_base": node_addr(r"(?:clint|aclint-mtimer)", DEFAULT_CLINT),
        "plic_base": node_addr(r"(?:plic|interrupt-controller)",
                               DEFAULT_PLIC),
        "uart_base": node_addr("serial", DEFAULT_UART),
        "num_harts": len(re.findall(r"cpu@\d+\s*{", dts)),
    }
    m = re.search(r"timebase-frequency\s*=\s*<\s*(0x[0-9a-fA-F]+|\d+)", dts)
    plat["timebase"] = int(m.group(1), 0) if m else 10000000
    if plat["num_harts"] < 1:
        plat["num_harts"] = smp
    return plat


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image-dir", default=os.path.join(REPO_ROOT, "images"))
    ap.add_argument("--out",
                    default=os.path.join(REPO_ROOT, "snapshots", "snap"),
                    help="output snapshot directory")
    ap.add_argument("--mode", choices=["bench", "philo", "shell"],
                    default="bench",
                    help="bench/philo: breakpoint barrier on the benchmark's "
                         "snapshot_barrier(); shell: marker barrier on the "
                         "idle shell")
    ap.add_argument("--mem-mb", type=int, default=256)
    ap.add_argument("--smp", type=int, default=1, help="number of harts")
    ap.add_argument("--cpu", default=DEFAULT_CPU)
    ap.add_argument("--gdb-port", type=int, default=11234)
    ap.add_argument("--boot-timeout", type=float, default=120.0)
    ap.add_argument("--break-symbol", default="snapshot_barrier",
                    help="(bench/philo mode) function to breakpoint on")
    ap.add_argument("--marker", default="QEMU-CPU-MODE-SHELL-READY",
                    help="(shell mode) serial-console string to snapshot on")
    ap.add_argument("--settle", type=float, default=2.0,
                    help="(shell mode) delay after the marker")
    ap.add_argument("--qemu", default="qemu-system-riscv64")
    args = ap.parse_args()

    img = args.image_dir
    kernel = os.path.join(img, "Image")
    initrd = os.path.join(img, "initramfs.cpio.gz")
    bios = os.path.join(img, "fw_jump.bin")
    # bench/philo capture breakpoints on a symbol inside the benchmark ELF;
    # the ELF basename matches the capture mode (/bin/bench, /bin/philo).
    bench_elf = os.path.join(img, "src", "rootfs", "bin", args.mode)
    for p in (kernel, initrd, bios):
        if not os.path.exists(p):
            sys.exit("missing image artifact: %s (run build-image.sh)" % p)

    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    run_dir = os.path.join(out, ".run")
    os.makedirs(run_dir, exist_ok=True)
    qmp_sock = os.path.join(run_dir, "qmp.sock")
    ser_sock = os.path.join(run_dir, "serial.sock")
    for s in (qmp_sock, ser_sock):
        if os.path.exists(s):
            os.unlink(s)

    # ---- device tree: derive the platform layout up front ----------------
    dtb_path = os.path.join(out, "virt.dtb")
    log("dumping + parsing the QEMU 'virt' device tree")
    plat = dump_and_parse_dtb(args.qemu, args.cpu, args.smp, args.mem_mb,
                              bios, dtb_path)
    mem_bytes = args.mem_mb * 1024 * 1024
    plat["ram_size"] = mem_bytes
    log("platform: ram=%#x+%dMiB clint=%#x plic=%#x uart=%#x harts=%d "
        "timebase=%d" % (plat["ram_base"], args.mem_mb, plat["clint_base"],
                         plat["plic_base"], plat["uart_base"],
                         plat["num_harts"], plat["timebase"]))

    # /init in the initramfs picks what to run from qemucpu.mode=.
    kcmd = "console=ttyS0 earlycon=sbi qemucpu.mode=" + args.mode

    qemu_cmd = [
        args.qemu, "-machine", "virt", "-cpu", args.cpu,
        "-smp", str(args.smp), "-m", "%dM" % args.mem_mb,
        "-bios", bios, "-kernel", kernel, "-initrd", initrd,
        "-append", kcmd, "-display", "none",
        "-chardev", "socket,id=ser0,path=%s,server=on,wait=off" % ser_sock,
        "-serial", "chardev:ser0",
        "-qmp", "unix:%s,server=on,wait=off" % qmp_sock,
        "-gdb", "tcp::%d" % args.gdb_port, "-no-reboot",
    ]

    log("launching QEMU (mode=%s, %d MiB, %d hart(s))"
        % (args.mode, args.mem_mb, args.smp))
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)
    gdb = None
    stop_evt = threading.Event()
    try:
        qmp = QMP(qmp_sock)
        ser = connect_unix(ser_sock)
        serial_log = os.path.join(out, "serial.log")

        if args.mode in ("bench", "philo"):
            # Race-free barrier: drain the console in the background while
            # gdb runs the guest to the breakpoint on snapshot_barrier().
            threading.Thread(target=serial_drainer,
                             args=(ser, serial_log, stop_evt),
                             daemon=True).start()
            addr = resolve_symbol(bench_elf, args.break_symbol)
            log("breakpoint barrier: %s @ %#x" % (args.break_symbol, addr))
            gdb = GdbDriver(args.gdb_port)
            gdb.set_breakpoint(addr)
            gdb.continue_to_breakpoint(args.boot_timeout)
            gdb.delete_breakpoints()
            log("barrier reached; pausing the VM")
            qmp.execute("stop")
        else:
            # Marker barrier: snapshot the idle interactive shell.
            found = threading.Event()
            threading.Thread(
                target=serial_drainer,
                args=(ser, serial_log, stop_evt, args.marker.encode(), found),
                daemon=True).start()
            log("waiting for marker %r ..." % args.marker)
            if not found.wait(timeout=args.boot_timeout):
                raise RuntimeError("timed out waiting for %r" % args.marker)
            time.sleep(args.settle)
            log("marker seen; pausing the VM")
            qmp.execute("stop")
            gdb = GdbDriver(args.gdb_port)

        # ---- dump physical memory + MMIO device state --------------------
        log("dumping guest RAM (%d MiB) -> ram.bin" % args.mem_mb)
        qmp.pmemsave(plat["ram_base"], mem_bytes,
                     os.path.join(out, "ram.bin"))
        log("dumping CLINT / PLIC / UART MMIO")
        qmp.pmemsave(plat["clint_base"], CLINT_SIZE,
                     os.path.join(out, "clint.bin"))
        qmp.pmemsave(plat["plic_base"], PLIC_SIZE,
                     os.path.join(out, "plic.bin"))
        qmp.pmemsave(plat["uart_base"], UART_SIZE,
                     os.path.join(out, "uart.bin"))

        # ---- dump per-hart registers via the gdbstub ---------------------
        log("dumping CPU registers / CSRs for %d hart(s)" % args.smp)
        regtext = gdb.dump_registers()
        harts = parse_regs(regtext)
        hart_meta = []
        for h in sorted(harts):
            fn = "regs.hart%d.txt" % h
            with open(os.path.join(out, fn), "w") as f:
                for name in sorted(harts[h]):
                    f.write("%s 0x%x\n" % (name, harts[h][name]))
            hart_meta.append({"hart": h, "regs_file": fn,
                              "pc": harts[h].get("pc", 0)})
            log("  hart %d: %d registers, pc=%#x"
                % (h, len(harts[h]), harts[h].get("pc", 0)))
        if not hart_meta:
            raise RuntimeError("no register state captured")

        meta = {
            "version": 2,
            "arch": "riscv64",
            "mode": args.mode,
            "qemu_cpu": args.cpu,
            "num_harts": len(hart_meta),
            "ram": {"base": plat["ram_base"], "size": mem_bytes,
                    "file": "ram.bin"},
            "clint": {"base": plat["clint_base"], "size": CLINT_SIZE,
                      "file": "clint.bin"},
            "plic": {"base": plat["plic_base"], "size": PLIC_SIZE,
                     "file": "plic.bin"},
            "uart": {"base": plat["uart_base"], "size": UART_SIZE,
                     "file": "uart.bin"},
            "timebase": plat["timebase"],
            "harts": hart_meta,
            "dtb_file": "virt.dtb",
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }
        with open(os.path.join(out, "meta.json"), "w") as f:
            json.dump(meta, f, indent=2)

        log("quitting QEMU")
        try:
            qmp.execute("quit")
        except (RuntimeError, OSError):
            pass
        gdb.quit()
        gdb = None
        qemu.wait(timeout=10)
    finally:
        stop_evt.set()
        if gdb is not None:
            gdb.quit()
        if qemu.poll() is None:
            qemu.kill()

    log("snapshot written to %s" % out)
    for name in sorted(os.listdir(out)):
        p = os.path.join(out, name)
        if os.path.isfile(p):
            print("    %-18s %10d bytes" % (name, os.path.getsize(p)))


if __name__ == "__main__":
    main()
