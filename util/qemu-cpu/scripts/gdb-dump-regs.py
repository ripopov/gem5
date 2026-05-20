# gdb-dump-regs.py - dump every CPU register of a stopped QEMU gdbstub
# target as raw 64-bit values.  Run from gdb (gdb-multiarch):
#
#   gdb-multiarch -nx -batch -x gdb-dump-regs.py
#
# (the caller is expected to have already issued "target remote ...").
#
# Output: one "REG <name> 0x<hex>" line per register, plus "REGDUMP-OK"
# or "REGDUMP-ERR <msg>".  f-registers are emitted as their raw bit
# pattern (read via Value.bytes), not as a decoded float.
import gdb


def main():
    gdb.execute("set pagination off", to_string=True)
    frame = gdb.selected_frame()
    arch = frame.architecture()
    for reg in arch.registers():
        try:
            val = frame.read_register(reg)
            raw = val.bytes  # little-endian raw bytes
            iv = int.from_bytes(raw, "little")
        except Exception:
            continue
        print("REG %s 0x%x" % (reg.name, iv))


try:
    main()
    print("REGDUMP-OK")
except Exception as exc:  # noqa: BLE001
    print("REGDUMP-ERR %s" % exc)
