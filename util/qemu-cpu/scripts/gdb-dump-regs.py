# gdb-dump-regs.py - dump every CPU register of every hart of a stopped
# QEMU gdbstub target.  Run by qemu-snapshot.py inside its gdb session
# (after `target remote ...` and the barrier has been reached):
#
#   (gdb) source gdb-dump-regs.py
#
# Output: one "REG <hart> <name> 0x<hex>" line per register per hart, then
# "REGDUMP-OK" (or "REGDUMP-ERR <msg>").  Each QEMU vCPU is a gdb thread, so
# enumerating threads enumerates harts.  GPRs, the f-registers (raw bit
# pattern, via Value.bytes) and every CSR are all emitted.
import gdb


def dump_thread(hart, thread):
    thread.switch()
    frame = gdb.selected_frame()
    arch = frame.architecture()
    for reg in arch.registers():
        try:
            val = frame.read_register(reg)
            iv = int.from_bytes(val.bytes, "little")
        except Exception:
            continue
        print("REG %d %s 0x%x" % (hart, reg.name, iv))


def main():
    gdb.execute("set pagination off", to_string=True)
    inferior = gdb.selected_inferior()
    threads = list(inferior.threads())
    if not threads:
        raise RuntimeError("no threads / harts on the target")
    for hart, thread in enumerate(threads):
        dump_thread(hart, thread)


try:
    main()
    print("REGDUMP-OK")
except Exception as exc:  # noqa: BLE001
    print("REGDUMP-ERR %s" % exc)
