#!/usr/bin/env python3
"""qemu-snapshot.py - boot the minimal RISC-V image under QEMU, run to the
shell, and capture a full machine snapshot for gem5 QEMU-CPU mode.

Pipeline stage 1 of 3:

    [qemu-snapshot.py]  ->  [qemu2gem5.py converter]  ->  [gem5 restore]

The snapshot directory it produces contains:

    ram.bin     raw guest DRAM image          (base 0x80000000)
    clint.bin   CLINT MMIO region dump        (base 0x02000000)
    plic.bin    PLIC  MMIO region dump        (base 0x0c000000)
    regs.txt    every CPU register + CSR      (from the QEMU gdbstub)
    serial.log  full boot console transcript
    meta.json   addresses / sizes / parameters tying it all together

QEMU is driven through three control planes simultaneously:
  * a UNIX-socket serial console (to detect the shell prompt),
  * the QMP monitor      (to pause the VM and dump physical memory),
  * the gdbstub          (to read the architectural CPU/CSR state).
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, "..", "..", ".."))

GUEST_RAM_BASE = 0x80000000
CLINT_BASE, CLINT_SIZE = 0x02000000, 0x10000
PLIC_BASE, PLIC_SIZE = 0x0C000000, 0x600000
SHELL_MARKER = b"QEMU-CPU-MODE-SHELL-READY"

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


def read_until(sock, marker, timeout, logpath):
    """Read from the serial socket until `marker` appears; tee to logpath."""
    sock.settimeout(1.0)
    buf = bytearray()
    deadline = time.time() + timeout
    with open(logpath, "wb") as logf:
        while time.time() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                raise RuntimeError("serial connection closed before marker")
            buf += chunk
            logf.write(chunk)
            logf.flush()
            if marker in buf:
                return bytes(buf)
    raise RuntimeError("timed out waiting for shell marker %r" % marker)


def dump_registers(gdb_port, out_path):
    """Read all CPU registers/CSRs through the QEMU gdbstub."""
    cmd = [
        "gdb-multiarch", "-nx", "-batch", "-q",
        "-ex", "set pagination off",
        "-ex", "set confirm off",
        "-ex", "target remote :%d" % gdb_port,
        "-x", os.path.join(SCRIPT_DIR, "gdb-dump-regs.py"),
        "-ex", "detach",
    ]
    res = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    regs = {}
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[0] == "REG":
            try:
                regs[parts[1]] = int(parts[2], 0)
            except ValueError:
                pass
    if "REGDUMP-OK" not in res.stdout:
        log("gdb stdout:\n" + res.stdout)
        log("gdb stderr:\n" + res.stderr)
        raise RuntimeError("register dump failed")
    with open(out_path, "w") as f:
        for name in sorted(regs):
            f.write("%s 0x%x\n" % (name, regs[name]))
    return regs


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image-dir", default=os.path.join(REPO_ROOT, "images"))
    ap.add_argument("--out", default=os.path.join(REPO_ROOT, "snapshots", "snap"),
                    help="output snapshot directory")
    ap.add_argument("--mem-mb", type=int, default=256)
    ap.add_argument("--cpu", default=DEFAULT_CPU)
    ap.add_argument("--gdb-port", type=int, default=11234)
    ap.add_argument("--boot-timeout", type=float, default=120.0)
    ap.add_argument("--marker", default="QEMU-CPU-MODE-BENCH-READY",
                    help="serial-console string to snapshot on. The default "
                         "is printed by /bin/bench right before its CPU-bound "
                         "region; use QEMU-CPU-MODE-SHELL-READY to snapshot "
                         "the idle shell instead.")
    ap.add_argument("--settle", type=float, default=0.0,
                    help="seconds to wait after the marker before snapshotting")
    ap.add_argument("--qemu", default="qemu-system-riscv64")
    args = ap.parse_args()

    img = args.image_dir
    kernel = os.path.join(img, "Image")
    initrd = os.path.join(img, "initramfs.cpio.gz")
    bios = os.path.join(img, "fw_jump.bin")
    for p in (kernel, initrd, bios):
        if not os.path.exists(p):
            sys.exit("missing image artifact: %s (run build-image.sh)" % p)

    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    ram_bin = os.path.join(out, "ram.bin")
    clint_bin = os.path.join(out, "clint.bin")
    plic_bin = os.path.join(out, "plic.bin")
    regs_txt = os.path.join(out, "regs.txt")
    serial_log = os.path.join(out, "serial.log")
    meta_json = os.path.join(out, "meta.json")

    run_dir = os.path.join(out, ".run")
    os.makedirs(run_dir, exist_ok=True)
    qmp_sock = os.path.join(run_dir, "qmp.sock")
    ser_sock = os.path.join(run_dir, "serial.sock")
    for s in (qmp_sock, ser_sock):
        if os.path.exists(s):
            os.unlink(s)

    mem_bytes = args.mem_mb * 1024 * 1024
    qemu_cmd = [
        args.qemu,
        "-machine", "virt",
        "-cpu", args.cpu,
        "-smp", "1",
        "-m", "%dM" % args.mem_mb,
        "-bios", bios,
        "-kernel", kernel,
        "-initrd", initrd,
        "-append", "console=ttyS0 earlycon=sbi",
        "-display", "none",
        "-chardev", "socket,id=ser0,path=%s,server=on,wait=off" % ser_sock,
        "-serial", "chardev:ser0",
        "-qmp", "unix:%s,server=on,wait=off" % qmp_sock,
        "-gdb", "tcp::%d" % args.gdb_port,
        "-no-reboot",
    ]

    log("launching QEMU (%d MB, cpu=%s)" % (args.mem_mb, args.cpu))
    qemu = subprocess.Popen(qemu_cmd, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE)
    try:
        # Connect QMP up front so that, the instant the marker appears, the
        # only thing between us and a paused VM is a single 'stop' command -
        # the benchmark barely advances during the capture window.
        qmp = QMP(qmp_sock)
        ser = connect_unix(ser_sock)
        marker = args.marker.encode()
        log("waiting for marker %r on the serial console ..." % args.marker)
        read_until(ser, marker, args.boot_timeout, serial_log)
        if args.settle > 0:
            time.sleep(args.settle)
        log("marker seen; pausing the VM")
        qmp.execute("stop")

        log("dumping guest RAM (%d MB) -> ram.bin" % args.mem_mb)
        qmp.pmemsave(GUEST_RAM_BASE, mem_bytes, ram_bin)
        log("dumping CLINT MMIO -> clint.bin")
        qmp.pmemsave(CLINT_BASE, CLINT_SIZE, clint_bin)
        log("dumping PLIC MMIO -> plic.bin")
        qmp.pmemsave(PLIC_BASE, PLIC_SIZE, plic_bin)

        log("dumping CPU registers / CSRs via gdbstub -> regs.txt")
        regs = dump_registers(args.gdb_port, regs_txt)
        log("captured %d registers (pc=0x%x)"
            % (len(regs), regs.get("pc", 0)))

        meta = {
            "version": 1,
            "arch": "riscv64",
            "qemu_cpu": args.cpu,
            "guest_ram_base": GUEST_RAM_BASE,
            "guest_ram_size": mem_bytes,
            "ram_file": "ram.bin",
            "clint_base": CLINT_BASE,
            "clint_size": CLINT_SIZE,
            "clint_file": "clint.bin",
            "plic_base": PLIC_BASE,
            "plic_size": PLIC_SIZE,
            "plic_file": "plic.bin",
            "regs_file": "regs.txt",
            "pc": regs.get("pc", 0),
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }
        with open(meta_json, "w") as f:
            json.dump(meta, f, indent=2)

        log("quitting QEMU")
        try:
            qmp.execute("quit")
        except RuntimeError:
            pass
        qemu.wait(timeout=10)
    finally:
        if qemu.poll() is None:
            qemu.kill()

    log("snapshot written to %s" % out)
    for name in ("ram.bin", "clint.bin", "plic.bin", "regs.txt", "meta.json"):
        p = os.path.join(out, name)
        sz = os.path.getsize(p) if os.path.exists(p) else 0
        print("    %-12s %10d bytes" % (name, sz))


if __name__ == "__main__":
    main()
