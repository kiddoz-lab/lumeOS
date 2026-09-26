#!/usr/bin/env python3
"""Embed a userspace ELF image in the kernel as a C array.

There is no filesystem yet, so the first user program has to reach the kernel
some other way.  Linking it into the kernel image is the same technique the
Linux kernel uses for an initramfs or a built-in init - the difference here is
that the blob is a single small static ELF rather than an archive, and that the
kernel's ELF loader does not care where the bytes came from.  When there *is* a
filesystem, /bin/init will arrive through the VFS and this tool disappears.

The generated file is deliberately committed to the build directory rather than
to the repository (see .gitignore): it is a build product, and the ELF it wraps
is itself a build product.

Usage:
    tools/embed_user.py build/userspace/init.elf --symbol lume_init_elf \
        --output build/init_blob.c
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

ELF_MAGIC = b"\x7fELF"
EM_ARM = 40
ET_EXEC = 2
ELFCLASS32 = 1
PT_LOAD = 1


def check_elf(image: bytes, path: Path) -> tuple[int, int]:
    """Validate the things the kernel's loader will validate, here.

    Doing it at build time means a broken userspace build fails the kernel
    build with a clear message instead of producing a kernel that panics (or
    worse, quietly skips its init program) on the board.
    """
    if len(image) < 52:
        raise SystemExit(f"embed_user: {path}: too small to be an ELF")
    if image[:4] != ELF_MAGIC:
        raise SystemExit(f"embed_user: {path}: not an ELF file")
    if image[4] != ELFCLASS32:
        raise SystemExit(f"embed_user: {path}: not ELFCLASS32")
    if image[5] != 1:
        raise SystemExit(f"embed_user: {path}: not little-endian")
    e_type, e_machine = struct.unpack_from("<HH", image, 16)
    if e_machine != EM_ARM:
        raise SystemExit(f"embed_user: {path}: e_machine={e_machine}, expected ARM ({EM_ARM})")
    if e_type != ET_EXEC:
        raise SystemExit(f"embed_user: {path}: e_type={e_type}, expected ET_EXEC ({ET_EXEC}); "
                         "a static PIE or a relocatable object cannot be loaded by "
                         "the kernel loader, which does not process relocations")
    e_entry, = struct.unpack_from("<I", image, 24)
    if e_entry < 0x1000:
        raise SystemExit(f"embed_user: {path}: entry 0x{e_entry:08x} is inside the "
                         "null page; user programs must be linked above it")

    check_phdrs(image, path, e_entry)
    return e_entry, e_machine


def check_phdrs(image: bytes, path: Path, e_entry: int) -> None:
    """The program headers have to be mapped, or AT_PHDR has no value.

    A program is told where its own program headers are by AT_PHDR, and the
    kernel computes that address the way Linux does: find the PT_LOAD whose file
    range contains e_phoff, and report `e_phoff - p_offset + p_vaddr`.  If no
    segment contains them, the headers are not in memory at all and AT_PHDR has
    to be 0 - which a C library cannot work with, since reading the headers is
    how it finds PT_TLS and PT_GNU_RELRO before it runs a line of the program.

    This is checked here because it is a property of the *linker script*, it is
    invisible in a listing (`readelf -l` happily shows three clean segments),
    and getting it wrong produces a kernel that boots and then kills the first
    program to look at its own headers.  It happened once: the text segment
    started at file offset 0x1000 with the headers unmapped, AT_PHDR came out as
    0x34, and init died with SIGSEGV before printing anything.
    """
    e_phoff, = struct.unpack_from("<I", image, 28)
    e_phentsize, e_phnum = struct.unpack_from("<HH", image, 42)
    if e_phentsize != 32 or e_phnum == 0:
        raise SystemExit(f"embed_user: {path}: e_phentsize={e_phentsize}, "
                         f"e_phnum={e_phnum}; the kernel reads 32-byte phdrs")
    if e_phoff + e_phnum * e_phentsize > len(image):
        raise SystemExit(f"embed_user: {path}: the program header table "
                         f"(0x{e_phoff:x}, {e_phnum} entries) runs past the file")

    phdr_addr = None
    entry_seg = None
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz, p_flags, _p_align = \
            struct.unpack_from("<8I", image, e_phoff + i * 32)
        if p_type != PT_LOAD:
            continue
        if p_offset <= e_phoff < p_offset + p_filesz:
            phdr_addr = e_phoff - p_offset + p_vaddr
        if p_vaddr <= e_entry < p_vaddr + p_memsz:
            entry_seg = (p_vaddr, p_memsz, p_flags)

    if phdr_addr is None:
        raise SystemExit(f"embed_user: {path}: no PT_LOAD segment contains the "
                         f"program headers (e_phoff=0x{e_phoff:x}); AT_PHDR would "
                         "be 0 and a C library could not start - check the linker "
                         "script, the first PT_LOAD needs FILEHDR PHDRS and the "
                         "text must start at the load address plus SIZEOF_HEADERS")
    if entry_seg is None:
        raise SystemExit(f"embed_user: {path}: the entry point 0x{e_entry:08x} is "
                         "not inside any PT_LOAD segment")
    if not entry_seg[2] & 1:
        raise SystemExit(f"embed_user: {path}: the entry point 0x{e_entry:08x} is "
                         f"not in an executable segment (p_flags={entry_seg[2]})")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("elf", type=Path)
    parser.add_argument("--symbol", default="lume_init_elf")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)

    image = args.elf.read_bytes()
    entry, _ = check_elf(image, args.elf)
    digest = hashlib.sha256(image).hexdigest()

    lines = [
        "/* Generated by tools/embed_user.py from %s.  Do not edit. */" % args.elf,
        "#include <lume/types.h>",
        "",
        f"const u8 {args.symbol}[] = {{",
    ]
    for i in range(0, len(image), 12):
        chunk = image[i:i + 12]
        lines.append("    " + " ".join(f"0x{b:02x}," for b in chunk))
    lines += [
        "};",
        "",
        f"const u32 {args.symbol}_size = {len(image)}u;",
        f"const u32 {args.symbol}_entry = 0x{entry:08x}u;",
        f'const char {args.symbol}_sha256[] = "{digest}";',
        "",
    ]

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines))
    print(f"embed_user: {args.output}: {len(image)} bytes from {args.elf} "
          f"(entry 0x{entry:08x}, sha256 {digest[:16]}...)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
