#!/usr/bin/env python3
"""Convert the linked LumeOS kernel ELF into the flat image the Raspberry Pi
boot firmware expects.

The Raspberry Pi boot firmware (bootcode.bin -> start.elf) loads `kernel.img`
into memory at a fixed physical address and jumps to its first byte.  For the
32-bit BCM2835 firmware that address is 0x8000, which is exactly where the
LumeOS linker script places the load memory address (LMA) of the kernel text.

This tool therefore:
  * reads the PT_LOAD program headers of the ELF file,
  * refuses to emit an image whose first load address is not the expected
    physical address (default 0x8000, overridable with --require-paddr),
  * rejects loadable segments that sit at a virtual-only address (a linker
    script without an explicit PHDRS statement produces such a segment),
  * warns when a segment would overlap the reserved exception vector page at
    physical 0xF0000,
  * writes the flat binary with gaps zero-filled, plus a human readable map on
    stderr so the build log shows the memory layout.

Usage:
    elf2bin.py build/lumeos.elf build/kernel.img --require-paddr 0x8000
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass

EM_ARM = 40
ET_EXEC = 2
PT_LOAD = 1

# Physical page reserved for the exception vector page (see kernel/ld/lumeos.ld).
VECTOR_PAGE_PA = 0xF0000

# Physical addresses on BCM2835 are below 0x40000000; anything above is either
# a typo or a virtual address leaking into p_paddr.
MAX_PHYS_ADDR = 0x40000000

# A kernel image that big cannot be right (the linker script asserts a limit of
# about 928 KiB); this guard only exists to fail fast and loudly.
MAX_IMAGE_SIZE = 16 * 1024 * 1024


class ElfError(Exception):
    pass


@dataclass
class Segment:
    offset: int
    vaddr: int
    paddr: int
    filesz: int
    memsz: int
    flags: int
    align: int
    data: bytes = b""

    @property
    def perm(self) -> str:
        return "".join(
            c if self.flags & bit else "-"
            for c, bit in (("R", 4), ("W", 2), ("X", 1))
        )

    @property
    def end(self) -> int:
        return self.paddr + self.memsz


def parse_elf(data: bytes) -> list[Segment]:
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise ElfError("not an ELF file")
    ei_class, ei_data = data[4], data[5]
    if ei_class != 1:
        raise ElfError("only 32-bit ELF files are supported (EI_CLASS != ELFCLASS32)")
    if ei_data != 1:
        raise ElfError("only little-endian ELF files are supported (EI_DATA != ELFDATA2LSB)")

    (e_type, e_machine, _e_version, _e_entry, e_phoff, _e_shoff, _e_flags,
     _e_ehsize, e_phentsize, e_phnum, _e_shentsize, _e_shnum,
     _e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)

    if e_machine != EM_ARM:
        raise ElfError(f"expected an ARM ELF file (e_machine=40), got {e_machine}")
    if e_type != ET_EXEC:
        raise ElfError(f"expected an executable ELF (ET_EXEC=2), got {e_type}")
    if e_phnum == 0:
        raise ElfError("ELF file has no program headers")

    segments: list[Segment] = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        if off + e_phentsize > len(data):
            raise ElfError("program header table is truncated")
        (p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags,
         p_align) = struct.unpack_from("<IIIIIIII", data, off)
        if p_type != PT_LOAD or p_memsz == 0:
            continue
        if p_offset + p_filesz > len(data):
            raise ElfError("segment points outside the file")
        if p_memsz < p_filesz:
            raise ElfError("segment memsz < filesz")
        segments.append(Segment(p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
                                p_flags, p_align, data[p_offset:p_offset + p_filesz]))

    if not segments:
        raise ElfError("ELF file has no PT_LOAD segments")
    segments.sort(key=lambda s: s.paddr)
    return segments


def build_image(segments: list[Segment], require_paddr: int | None) -> bytes:
    for seg in segments:
        if seg.paddr >= MAX_PHYS_ADDR:
            raise ElfError(
                f"PT_LOAD segment at 0x{seg.paddr:x} is not a physical address.  "
                "This normally means the linker script produced a virtual-only "
                "header segment; add an explicit PHDRS/section-to-segment "
                "assignment (see kernel/ld/lumeos.ld)."
            )
        if seg.paddr + seg.memsz >= MAX_PHYS_ADDR:
            raise ElfError(f"PT_LOAD segment at 0x{seg.paddr:x} has an implausible size")

    if require_paddr is not None and segments[0].paddr != require_paddr:
        raise ElfError(
            f"kernel load address is 0x{segments[0].paddr:x}, but the Raspberry Pi "
            f"firmware loads kernel.img at 0x{require_paddr:x} "
            "(check the LMA in kernel/ld/lumeos.ld)"
        )

    base = segments[0].paddr
    end = max(s.end for s in segments)
    size = end - base
    if size > MAX_IMAGE_SIZE:
        raise ElfError(
            f"flat image would be {size} bytes ({size // (1024 * 1024)} MiB); "
            "the kernel is expected to be smaller than 1 MiB"
        )

    for seg in segments:
        if seg.paddr < VECTOR_PAGE_PA < seg.end:
            raise ElfError(
                f"segment at 0x{seg.paddr:x}+0x{seg.memsz:x} overlaps the "
                f"exception vector page at 0x{VECTOR_PAGE_PA:x}; the kernel image "
                "is too large (see the ASSERT in kernel/ld/lumeos.ld)"
            )

    image = bytearray(size)
    for seg in segments:
        start = seg.paddr - base
        image[start:start + seg.filesz] = seg.data[:seg.filesz]
        # .bss (memsz > filesz) stays zero because the buffer starts zeroed.
    return bytes(image)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("elf", help="input ELF file (build/lumeos.elf)")
    parser.add_argument("output", help="output flat binary (build/kernel.img)")
    parser.add_argument("--require-paddr", default=None,
                        help="fail unless the first segment loads at this address "
                             "(e.g. 0x8000); the Makefile passes 0x8000")
    args = parser.parse_args(argv)

    require = int(args.require_paddr, 0) if args.require_paddr else None

    try:
        with open(args.elf, "rb") as fh:
            data = fh.read()
        segments = parse_elf(data)
        image = build_image(segments, require)
    except (ElfError, OSError) as exc:
        print(f"elf2bin: error: {exc}", file=sys.stderr)
        return 1

    try:
        with open(args.output, "wb") as fh:
            fh.write(image)
    except OSError as exc:
        print(f"elf2bin: error: cannot write {args.output}: {exc}", file=sys.stderr)
        return 1

    print(f"elf2bin: {args.output}: {len(image)} bytes (0x{len(image):x}), "
          f"base physical address 0x{segments[0].paddr:x}", file=sys.stderr)
    for seg in segments:
        origin = "file-backed" if seg.filesz else "zero-filled"
        print(f"elf2bin:   load 0x{seg.paddr:08x}..0x{seg.end:08x} "
              f"({seg.memsz:6d} bytes, {seg.perm}, {origin})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
