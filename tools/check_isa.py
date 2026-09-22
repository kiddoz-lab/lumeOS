#!/usr/bin/env python3
"""Verify that a linked LumeOS binary only contains instructions the
ARM1176JZF-S (ARMv6KZ) can execute.

Why this exists: the Raspberry Pi Zero W's CPU is an ARM1176JZF-S, which
implements ARMv6KZ.  Anything the compiler emits for ARMv7-A or later (SDIV,
CLREX, the ARMv7 barrier mnemonics, Thumb-2 only encodings, VFP/NEON when not
asked for) faults as an undefined instruction on real hardware, but can pass
unnoticed in emulation or on a newer board.  The build runs this check on the
kernel and on every userspace binary.

Two independent checks are performed:

  1. the ELF build attributes are read with `readelf -A`; Tag_CPU_arch must be
     an architecture the ARM1176 implements (v6, v6K, v6KZ);
  2. every executable section is disassembled with capstone (falling back to
     objdump) and scanned for mnemonics that ARMv6 does not have.

Usage:
    check_isa.py build/lumeos.elf
    check_isa.py userspace/bin/sh
"""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
from pathlib import Path

SHF_EXECINSTR = 0x4
SHT_NOBITS = 8

# Architectures the ARM1176JZF-S implements (Tag_CPU_arch values).
ALLOWED_CPU_ARCH = {
    "v6": 6,
    "v6K": 9,
    "v6KZ": 7,
    "6": 6,
    "6K": 9,
    "6KZ": 7,
}

# Mnemonics that do not exist on ARMv6, or that only exist in their ARMv7 form.
ARMV7_ONLY = {
    "sdiv", "udiv", "clrex", "dmb", "dsb", "isb", "sev", "wfe", "pldw",
    "ldrexb", "ldrexh", "ldrexd", "strexb", "strexh", "strexd",
    "bfc", "bfi", "sbfx", "ubfx",
    "cbz", "cbnz", "tbb", "tbh", "rev16", "revsh",
    "movw", "movt", "lda", "ldab", "ldah", "ldaex", "stl", "stlb", "stlh",
    "vldr", "vstr", "vldm", "vstm", "vpush", "vpop", "vmov", "vadd", "vsub",
    "vmul", "vdiv", "vcvt", "vmla", "vmrs", "vmsr", "vabs", "vneg", "vcmp",
    "sxtb16", "uxtb16", "sxtab", "uxtab", "sxtah", "uxtah",
}

# The ARMv7 barrier/hint mnemonics double as CP15 instruction names in ARMv6
# disassemblers; report them with the ARMv6 alternative spelled out.
BARRIER_HINTS = ("dmb", "dsb", "isb", "wfe", "sev", "clrex")


class CheckError(Exception):
    pass


def sections(path: Path) -> list[tuple[str, int, int, int]]:
    """Return (name, type, flags, offset, size) for each section."""
    data = path.read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1:
        raise CheckError("not a 32-bit ELF file")
    (_type, _machine, _ver, _entry, _phoff, e_shoff, _flags, _ehsize,
     _phentsize, _phnum, e_shentsize, e_shnum,
     e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    if e_shoff == 0 or e_shnum == 0:
        raise CheckError("ELF file has no section headers")

    strtab_entry = e_shoff + e_shstrndx * e_shentsize
    _n, _t, _f, _a, shstr_off, shstr_size = struct.unpack_from("<IIIIII", data, strtab_entry)
    shstr = data[shstr_off:shstr_off + shstr_size]

    result = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name_off, sh_type, sh_flags, _addr, sh_off, sh_size = struct.unpack_from(
            "<IIIIII", data, off)
        name = shstr[name_off:shstr.find(b"\0", name_off)].decode("ascii", "replace")
        result.append((name, sh_type, sh_flags, sh_off, sh_size))
    return result


def executable_bytes(path: Path) -> bytes:
    """Concatenate the contents of the executable sections."""
    data = path.read_bytes()
    out = bytearray()
    for _name, sh_type, sh_flags, sh_off, sh_size in sections(path):
        if sh_type == SHT_NOBITS or not sh_flags & SHF_EXECINSTR or sh_size == 0:
            continue
        out += data[sh_off:sh_off + sh_size]
    return bytes(out)


def attributes_with_readelf(path: Path) -> dict[str, str]:
    readelf = None
    for candidate in ("arm-none-eabi-readelf", "llvm-readelf", "readelf"):
        if shutil.which(candidate):
            readelf = candidate
            break
    if readelf is None:
        return {}

    proc = subprocess.run([readelf, "-A", str(path)], capture_output=True, text=True)
    if proc.returncode != 0:
        return {}
    out: dict[str, str] = {}
    for line in proc.stdout.splitlines():
        line = line.strip()
        if line.startswith("Tag_CPU_name:"):
            out["cpu_name"] = line.split(":", 1)[1].strip().strip('"')
        elif line.startswith("Tag_CPU_arch:"):
            out["cpu_arch"] = line.split(":", 1)[1].strip()
    return out


class CapstoneMissing(CheckError):
    """Raised when the ISA check cannot run because capstone is not installed."""


def disassemble(code: bytes) -> list[tuple[int, str]]:
    try:
        import capstone  # type: ignore
    except ImportError as exc:
        raise CapstoneMissing(
            "capstone is not installed, so the instruction set cannot be checked"
        ) from exc

    md = capstone.Cs(capstone.CS_ARCH_ARM,
                     capstone.CS_MODE_ARM | capstone.CS_MODE_LITTLE_ENDIAN)
    md.detail = False
    instructions = []
    for insn in md.disasm(code, 0):
        instructions.append((insn.address, insn.mnemonic.lower()))
    return instructions


def check_instructions(instructions: list[tuple[int, str]]) -> list[str]:
    problems: list[str] = []
    reported: set[str] = set()

    for address, mnemonic in instructions:
        base = mnemonic.split(".")[0].split()[0]
        if base in ARMV7_ONLY:
            key = f"{base}"
            if key in reported:
                continue
            reported.add(key)
            problems.append(
                f"0x{address:08x}: '{mnemonic}' does not exist on ARMv6 "
                "(ARM1176JZF-S)")
            continue
        for prefix in BARRIER_HINTS:
            if base == prefix:
                problems.append(
                    f"0x{address:08x}: '{mnemonic}' is the ARMv7 hint mnemonic; "
                    "ARMv6 must use the CP15 form (see kernel/include/lume/asm.h)")
                break
    return problems


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("elf")
    parser.add_argument("--require-attributes", action="store_true",
                        help="fail when the build attributes cannot be read")
    parser.add_argument("--require-capstone", action="store_true",
                        help="fail when capstone is missing instead of skipping the "
                             "instruction level check (CI always passes this: a "
                             "silently skipped ISA check is a false pass)")
    parser.add_argument("-q", "--quiet", action="store_true")
    args = parser.parse_args(argv)

    path = Path(args.elf)
    if not path.is_file():
        print(f"check_isa: {path}: no such file", file=sys.stderr)
        return 2

    failures: list[str] = []

    attrs = attributes_with_readelf(path)
    if attrs:
        arch = attrs.get("cpu_arch", "")
        if arch and arch not in ALLOWED_CPU_ARCH:
            failures.append(
                f"Tag_CPU_arch is '{arch}' but the ARM1176JZF-S implements "
                "ARMv6KZ; rebuild with -march=armv6kz -mcpu=arm1176jzf-s")
        name = attrs.get("cpu_name", "")
        if name and "arm1176" not in name.lower() and "arm11" not in name.lower():
            failures.append(
                f"Tag_CPU_name is '{name}', which is not an ARM1176/ARM11 core")
    elif args.require_attributes:
        failures.append("no ARM build attributes found (is this an ARM ELF?)")

    try:
        code = executable_bytes(path)
    except (CheckError, OSError) as exc:
        print(f"check_isa: {path}: {exc}", file=sys.stderr)
        return 1

    instructions: list[tuple[int, str]] = []
    try:
        instructions = disassemble(code)
    except CapstoneMissing as exc:
        if args.require_capstone:
            failures.append(str(exc))
        else:
            print(f"check_isa: warning: {exc}; "
                  "only the build attributes were verified", file=sys.stderr)
    failures.extend(check_instructions(instructions))

    for failure in failures:
        print(f"check_isa: FAIL: {failure}", file=sys.stderr)

    if failures:
        return 1

    if not args.quiet:
        detail = attrs.get("cpu_arch", "?")
        if attrs.get("cpu_name"):
            detail += f", {attrs['cpu_name']}"
        print(f"check_isa: {path}: ok ({len(instructions)} instructions in "
              f"{len(code)} bytes scanned; build attributes: {detail})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
