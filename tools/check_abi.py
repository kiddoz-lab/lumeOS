#!/usr/bin/env python3
"""Check the fixed-convention ARM EABI helpers in a linked LumeOS kernel ELF.

Why this exists
---------------

The compiler emits calls to the division helpers (`__aeabi_uidiv`,
`__aeabi_uldivmod`, ...) with a register layout fixed by the public "Run-time
ABI for the ARM Architecture" addendum: operands in r0-r3 and, for the
multi-word helpers, results back in r0:r1 (quotient) and r2:r3 (remainder).

That layout is *not* the convention this toolchain uses for equivalent C
functions: clang/LLVM returns every composite type in memory through a hidden
pointer in r0 (sret).  A helper written as

    struct u64_divmod __aeabi_uldivmod(u64 num, u64 den);

therefore reads its divisor from a stack slot the caller never wrote and writes
its result through a pointer the caller never passed.  Both halves compile
without a warning and the mismatch is visible only in the machine code.  It
cost LumeOS one boot in QEMU, where the kernel panicked with "64-bit division
by zero" before the serial console existed.

This tool looks for that class of bug specifically:

  * a helper's body must not store its result through r0 - that means its
    author (or its compiler) used the sret convention;
  * a call site must not dereference r0 immediately after calling a multi-word
    helper - that means the caller expected a result pointer there.

It is a static check on the linked ELF: a second on any machine with capstone,
no QEMU and no ARM CPU required.

The kernel must define all six helpers, because any C code in it may call one.
A userspace program is a different case: it is linked with -nostdlib, so if it
ever calls a helper the link fails unless the program defines it itself - the
linker, not this tool, is what guarantees closure there.  `--allow-none` accepts
an image that defines none of them (and then there is nothing to check), while
still checking the layout of any helper that *is* defined.

Usage:
    tools/check_abi.py build/lumeos.elf [--require-capstone] [--allow-none] [-q]
"""

from __future__ import annotations

import argparse
import bisect
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

# The rtabi helpers that must be reachable through the fixed EABI layout.
RTABI_HELPERS = (
    "__aeabi_uidiv",
    "__aeabi_idiv",
    "__aeabi_uidivmod",
    "__aeabi_idivmod",
    "__aeabi_uldivmod",
    "__aeabi_ldivmod",
)

# Helpers whose result is multi-word and therefore must come back in r0-r3.
MULTIWORD_HELPERS = ("__aeabi_uidivmod", "__aeabi_idivmod",
                     "__aeabi_uldivmod", "__aeabi_ldivmod")

STORE_MNEMONICS = ("str", "strb", "strh", "strd", "stm", "stmia")

SHF_EXECINSTR = 0x4
SHT_NOBITS = 8


class CheckError(Exception):
    """Raised when the ELF cannot be inspected at all."""


class CapstoneMissing(CheckError):
    """Raised when the checker cannot run because capstone is not installed."""


# --------------------------------------------------------------------------
# ELF helpers
# --------------------------------------------------------------------------

def _section_table(path: Path) -> list[tuple[str, int, int, int, int]]:
    """Return (name, type, flags, address, offset, size) for every section."""
    data = path.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4] != 1:
        raise CheckError("not a 32-bit ELF file")
    # ELF32 header layout: e_shoff@32, e_flags@36, e_ehsize@40,
    # e_phentsize@42, e_phnum@44, e_shentsize@46, e_shnum@48, e_shstrndx@50.
    e_shoff = struct.unpack_from("<I", data, 32)[0]
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 46)
    if e_shoff == 0 or e_shnum == 0:
        raise CheckError("ELF file has no section headers")

    shstr_off, shstr_size = struct.unpack_from(
        "<II", data, e_shoff + e_shstrndx * e_shentsize + 16)
    shstr = data[shstr_off:shstr_off + shstr_size]

    out = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        name_off, sh_type, sh_flags, sh_addr, sh_off, sh_size = struct.unpack_from(
            "<IIIIII", data, off)
        end = shstr.find(b"\0", name_off)
        name = shstr[name_off:end].decode("ascii", "replace")
        out.append((name, sh_type, sh_flags, sh_addr, sh_off, sh_size))
    return out


def executable_chunks(path: Path) -> list[tuple[int, bytes]]:
    """Return (virtual address, contents) for each executable section."""
    data = path.read_bytes()
    out = []
    for _name, sh_type, sh_flags, sh_addr, sh_off, sh_size in _section_table(path):
        if sh_type == SHT_NOBITS or not sh_flags & SHF_EXECINSTR or sh_size == 0:
            continue
        out.append((sh_addr, data[sh_off:sh_off + sh_size]))
    return out


def symbols(path: Path) -> list[tuple[int, int, str]]:
    """Return (address, size, name) for the defined functions in the ELF."""
    readelf = None
    for candidate in ("arm-none-eabi-readelf", "llvm-readelf", "readelf"):
        if shutil.which(candidate):
            readelf = candidate
            break
    if readelf is None:
        raise CheckError("no readelf found (install binutils)")

    proc = subprocess.run([readelf, "-sW", str(path)], capture_output=True, text=True)
    if proc.returncode != 0:
        raise CheckError(f"{readelf} -s failed: {proc.stderr.strip()}")

    out = []
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) < 8 or parts[3] != "FUNC":
            continue
        if not re.fullmatch(r"[0-9a-f]{8}", parts[1]) or not parts[2].isdigit():
            continue
        out.append((int(parts[1], 16), int(parts[2]), parts[7]))
    return sorted(out)


# --------------------------------------------------------------------------
# Instruction analysis
# --------------------------------------------------------------------------

def disassemble(code: bytes, base: int = 0):
    """Disassemble ARM code, skipping embedded data instead of giving up.

    capstone's ARM decoder stops at the first word it cannot decode, so a
    literal pool inside .text truncates the scan (that is how the ISA checker
    ended up inspecting 235 of 10231 instructions).  `skipdata` keeps going and
    emits `.byte` pseudo-instructions, which are filtered out here.
    """
    try:
        import capstone  # type: ignore
    except ImportError as exc:
        raise CapstoneMissing(
            "capstone is not installed, so the EABI helpers cannot be checked"
        ) from exc

    md = capstone.Cs(capstone.CS_ARCH_ARM,
                     capstone.CS_MODE_ARM | capstone.CS_MODE_LITTLE_ENDIAN)
    md.detail = False
    md.skipdata = True
    return [insn for insn in md.disasm(code, base) if insn.id != 0]


def stores_through_r0(insns) -> str | None:
    """Return the first instruction that stores to the address r0 holds.

    That is the signature of the compiler's sret convention for composite
    returns: r0 arrives holding a destination pointer and the result is written
    through it.  An EABI division helper instead receives an operand in r0.
    """
    for insn in insns:
        mnemonic = insn.mnemonic.lower()
        if mnemonic not in STORE_MNEMONICS:
            continue
        op_str = insn.op_str.strip()
        if mnemonic.startswith("stm"):
            if op_str.startswith("r0") and not op_str.startswith("r0!"):
                return f"{insn.address:#010x}  {mnemonic}\t{op_str}"
            continue
        if "[" in op_str and op_str.split("[", 1)[1].startswith("r0"):
            return f"{insn.address:#010x}  {mnemonic}\t{op_str}"
    return None


def call_sites(insns, targets: dict[int, str]):
    """Yield (instruction, callee name) for every branch to a target."""
    for insn in insns:
        if insn.mnemonic not in ("bl", "b", "blx"):
            continue
        op_str = insn.op_str.strip()
        if not op_str.startswith("#"):
            continue
        try:
            target = int(op_str[1:], 16)
        except ValueError:
            continue
        if target in targets:
            yield insn, targets[target]


def problems_for(insns, funcs) -> list[str]:
    """Return the ABI problems in a disassembled function table.

    `insns` is a list of capstone instructions sorted by address and `funcs` is
    a list of (address, size, name).  Keeping this separate from the ELF
    plumbing lets tests/host feed it hand-encoded instruction sequences.
    """
    problems: list[str] = []
    if not insns:
        return problems

    by_addr = {insn.address: insn for insn in insns}
    addresses = sorted(by_addr)

    for start, size, name in funcs:
        end = start + (size if size else 256)
        body = [by_addr[a] for a in addresses
                if start <= a < end and start <= by_addr[a].address]
        offender = stores_through_r0(body[:24])
        if offender:
            problems.append(
                f"{name}: stores its result through r0 ({offender}) - that is the "
                "compiler's sret convention for composite returns, not the EABI "
                "rtabi convention this symbol must implement")

    targets = {addr: name for addr, _size, name in funcs
               if name in RTABI_HELPERS}
    for insn, callee in call_sites(insns, targets):
        i = bisect.bisect_right(addresses, insn.address)
        for j in range(i, min(i + 2, len(addresses))):
            nxt = by_addr[addresses[j]]
            op_str = nxt.op_str.strip()
            if callee in MULTIWORD_HELPERS and "[r0]" in op_str \
                    and nxt.mnemonic.lower().startswith(("ldr", "str")):
                problems.append(
                    f"{callee} call at {insn.address:#010x}: the caller dereferences "
                    f"r0 immediately afterwards ({nxt.mnemonic} {op_str}), which is "
                    "not the EABI multi-word result convention")
    return problems


def helper_problems(path: Path, allow_none: bool = False) -> tuple[list[str], bool]:
    """Return the ABI problems found in `path`, and whether any helper exists.

    With `allow_none`, an image that defines no helper at all is reported as
    "nothing to check" rather than as a failure; a partially defined set is
    still an error, because that means the image was meant to provide them.
    """
    funcs = [(addr, size, name) for addr, size, name in symbols(path)
             if name in RTABI_HELPERS]
    problems: list[str] = []
    defined = {name for _a, _s, name in funcs}

    if not defined and allow_none:
        return [], False

    missing = sorted(set(RTABI_HELPERS) - defined)
    if missing:
        problems.append("missing EABI helper(s): " + ", ".join(missing))

    insns = []
    for addr, chunk in executable_chunks(path):
        insns.extend(disassemble(chunk, addr))
    insns.sort(key=lambda insn: insn.address)
    return problems + problems_for(insns, funcs), bool(defined)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--require-capstone", action="store_true",
                        help="fail when capstone is missing instead of skipping "
                             "the check")
    parser.add_argument("--allow-none", action="store_true",
                        help="accept an image that defines no EABI helper "
                             "(a freestanding userspace program linked with "
                             "-nostdlib); any helper it does define is still "
                             "checked")
    parser.add_argument("-q", "--quiet", action="store_true")
    args = parser.parse_args(argv)

    if not args.elf.is_file():
        print(f"check_abi: {args.elf}: no such file", file=sys.stderr)
        return 2

    try:
        problems, has_helpers = helper_problems(args.elf, args.allow_none)
    except CapstoneMissing as exc:
        if args.require_capstone:
            print(f"check_abi: FAIL: {exc}", file=sys.stderr)
            return 1
        print(f"check_abi: warning: {exc} (skipping EABI check)")
        return 0
    except (CheckError, OSError) as exc:
        print(f"check_abi: {args.elf}: {exc}", file=sys.stderr)
        return 1

    if problems:
        print(f"check_abi: FAIL: {args.elf}", file=sys.stderr)
        for problem in problems:
            print(f"check_abi:   {problem}", file=sys.stderr)
        return 1

    if not args.quiet:
        if has_helpers:
            print(f"check_abi: {args.elf}: ok (EABI helpers use the rtabi layout)")
        else:
            print(f"check_abi: {args.elf}: ok (no EABI helper in this image; "
                  "the link is what guarantees closure)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
