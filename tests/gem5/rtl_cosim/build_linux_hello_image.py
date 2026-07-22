#!/usr/bin/env python3
"""Overlay the JitCPU-to-C910 hello payload on a gem5 nodisk image."""

import argparse
import gzip
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import zlib


MAGIC = b"070701"
HEADER_SIZE = 110
ORIGINAL_INIT = b"#!/bin/busybox sh\n\n/sbin/m5 exit\n"
DEMO_INIT = (
    b"#!/bin/sh\n"
    b"exec /bin/rtl-hello\n"
)


def align4(value):
    return (value + 3) & ~3


def parse_newc(archive):
    """Return newc entries as (name, header, data, next) offsets."""
    entries = []
    offset = 0
    while archive[offset : offset + 6] == MAGIC:
        header = offset
        namesize = int(archive[offset + 94 : offset + 102], 16)
        filesize = int(archive[offset + 54 : offset + 62], 16)
        name_start = offset + HEADER_SIZE
        name_end = name_start + namesize
        if namesize == 0 or name_end > len(archive):
            raise ValueError("malformed newc filename")
        name = archive[name_start : name_end - 1]
        data_start = align4(name_end)
        data_end = data_start + filesize
        next_offset = align4(data_end)
        if next_offset > len(archive):
            raise ValueError("malformed newc file data")
        entries.append((name, header, data_start, data_end, next_offset))
        offset = next_offset
        if name == b"TRAILER!!!":
            return entries
    raise ValueError("newc archive has no TRAILER!!! entry")


def newc_entry(name, contents, inode, mode, rdevmajor=0, rdevminor=0):
    encoded_name = os.fsencode(name) + b"\0"
    fields = (
        inode,
        mode,
        0,
        0,
        2 if stat.S_ISDIR(mode) else 1,
        0,
        len(contents),
        0,
        0,
        rdevmajor,
        rdevminor,
        len(encoded_name),
        0,
    )
    header = MAGIC + b"".join(f"{value:08X}".encode() for value in fields)
    record = header + encoded_name
    record += b"\0" * (align4(len(record)) - len(record))
    record += contents
    record += b"\0" * (align4(len(record)) - len(record))
    return record


def find_initramfs(image):
    """Find the gzip member containing the embedded newc initramfs."""
    offset = 0
    while True:
        offset = image.find(b"\x1f\x8b\x08", offset)
        if offset < 0:
            break
        decompressor = zlib.decompressobj(16 + zlib.MAX_WBITS)
        try:
            archive = decompressor.decompress(image[offset:])
            archive += decompressor.flush()
        except zlib.error:
            offset += 1
            continue
        consumed = len(image[offset:]) - len(decompressor.unused_data)
        if archive.startswith(MAGIC):
            try:
                entries = parse_newc(archive)
            except ValueError:
                pass
            else:
                return offset, consumed, archive, entries
        offset += 1
    raise ValueError("no gzip-compressed newc initramfs found")


def compile_hello(source, compiler, output):
    command = [
        compiler,
        "-Os",
        "-march=rv64gc_zicsr_zifencei",
        "-mabi=lp64d",
        "-static",
        "-nostdlib",
        "-nostartfiles",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-pie",
        "-no-pie",
        "-fno-stack-protector",
        "-Wl,--build-id=none",
        "-Wl,-e,_start",
        "-o",
        str(output),
        str(source),
    ]
    subprocess.run(command, check=True)


def build_image(base_image, output, hello_source, compiler):
    image = base_image.read_bytes()
    gzip_offset, old_size, archive, entries = find_initramfs(image)

    init_entries = [entry for entry in entries if entry[0] == b"sbin/init"]
    if len(init_entries) != 1:
        raise ValueError("expected exactly one sbin/init in the initramfs")
    _, _, init_start, init_end, _ = init_entries[0]
    if archive[init_start:init_end] != ORIGINAL_INIT:
        raise ValueError("base image has an unexpected sbin/init payload")
    trailer_entries = [entry for entry in entries if entry[0] == b"TRAILER!!!"]
    if len(trailer_entries) != 1:
        raise ValueError("expected exactly one newc trailer")
    trailer_start = trailer_entries[0][1]

    with tempfile.TemporaryDirectory(prefix="rtl-cpu-linux-hello-") as tmp:
        hello = Path(tmp) / "rtl-hello"
        compile_hello(hello_source, compiler, hello)
        hello_contents = hello.read_bytes()

    init_header = init_entries[0][1]
    init_next = init_entries[0][4]
    overlay = b"".join(
        (
            newc_entry(
                "sbin/init",
                DEMO_INIT,
                0xC900,
                stat.S_IFREG | 0o755,
            ),
            archive[init_next:trailer_start],
            newc_entry(
                "dev", b"", 0xC901, stat.S_IFDIR | 0o755
            ),
            newc_entry(
                "dev/console",
                b"",
                0xC902,
                stat.S_IFCHR | 0o600,
                rdevmajor=5,
                rdevminor=1,
            ),
            newc_entry(
                "dev/ttyS0",
                b"",
                0xC903,
                stat.S_IFCHR | 0o600,
                rdevmajor=4,
                rdevminor=64,
            ),
            newc_entry(
                "bin/rtl-hello",
                hello_contents,
                0xC910,
                stat.S_IFREG | 0o755,
            ),
        )
    )
    updated = archive[:init_header] + overlay + archive[trailer_start:]
    compressed = gzip.compress(updated, compresslevel=9, mtime=0)
    if len(compressed) > old_size:
        raise ValueError(
            "updated initramfs does not fit its fixed kernel region: "
            f"{len(compressed)} > {old_size} bytes"
        )

    patched = bytearray(image)
    patched[gzip_offset : gzip_offset + len(compressed)] = compressed
    patched[gzip_offset + len(compressed) : gzip_offset + old_size] = (
        b"\0" * (old_size - len(compressed))
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(patched)
    output.chmod(base_image.stat().st_mode)

    _, _, check_archive, check_entries = find_initramfs(output.read_bytes())
    names = {entry[0] for entry in check_entries}
    required = {
        b"bin/rtl-hello",
        b"dev",
        b"dev/console",
        b"dev/ttyS0",
        b"sbin/init",
    }
    if not required.issubset(names):
        raise RuntimeError("generated image is missing overlay entries")
    check_init = next(
        entry for entry in check_entries if entry[0] == b"sbin/init"
    )
    if check_archive[check_init[2] : check_init[3]] != DEMO_INIT:
        raise RuntimeError("generated image has the wrong sbin/init")

    print(f"Created {output}")
    print(
        "Embedded initramfs: "
        f"{len(compressed)} bytes compressed, {len(updated)} bytes raw"
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base_image", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--hello-source",
        type=Path,
        default=Path(__file__).with_name("linux_hello.c"),
    )
    parser.add_argument("--cc", default="riscv64-linux-gnu-gcc")
    args = parser.parse_args()

    if not args.base_image.is_file():
        parser.error(f"base image not found: {args.base_image}")
    if not args.hello_source.is_file():
        parser.error(f"hello source not found: {args.hello_source}")
    build_image(
        args.base_image.resolve(),
        args.output.resolve(),
        args.hello_source.resolve(),
        args.cc,
    )


if __name__ == "__main__":
    main()
