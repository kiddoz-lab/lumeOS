#!/usr/bin/env python3
"""Find out where a LumeOS boot stops under QEMU.

`run_qemu_test.py` says *that* the kernel did not boot.  This script says
*why*, as far as an emulator can:

  1. runs QEMU with ``-d in_asm,int,unimp,guest_errors,cpu_reset`` so that the
     log records every translated block the guest executed, every exception it
     took, and any access to unimplemented or unassigned addresses;
  2. resolves the last executed addresses back to functions and source lines
     with ``arm-none-eabi-addr2line`` against build/lumeos.elf, so the log ends
     with something like ``0xc0008234  boot.S:96 (_start_high)`` instead of a
     hex number;
  3. reports the serial output size (a kernel that prints nothing and a kernel
     that prints unreadable junk are very different bugs).

Every line it prints is prefixed with ``DIAG`` so CI can lift the report into an
annotation (see tools/ci_annotate.py).

Usage:
    tests/qemu/diagnose_boot.py --image build/kernel.img --elf build/lumeos.elf
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

ADDRESS_RE = re.compile(r"^(0x[0-9a-fA-F]{4,16}):")
KERNEL_BIAS = 0xC0000000          # kernel is linked at 0xC0008000 / loaded at 0x8000


def find_tool(name: str) -> str | None:
    for candidate in (f"arm-none-eabi-{name}", f"llvm-{name}", name):
        path = shutil.which(candidate)
        if path:
            return path
    return None


def symbolize(addresses: list[int], elf: Path) -> dict[int, str]:
    """Resolve addresses to "file:line (function)" using addr2line."""
    addr2line = find_tool("addr2line")
    if addr2line is None or not elf.is_file() or not addresses:
        return {}

    def run(addrs: list[int]) -> list[str]:
        proc = subprocess.run([addr2line, "-f", "-e", str(elf)]
                              + [f"0x{a:x}" for a in addrs],
                              capture_output=True, text=True)
        lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
        # addr2line prints function, then location, per address
        return [" ".join(lines[i:i + 2]) for i in range(0, len(lines), 2)]

    # Guest addresses below 0x80000000 are physical (before the MMU is on);
    # the same code is linked 0xC0000000 higher, so try both mappings.
    biased = [a + KERNEL_BIAS if a < 0x80000000 else a for a in addresses]
    plain = run(addresses)
    high = run(biased)

    resolved: dict[int, str] = {}
    for addr, low, up in zip(addresses, plain, high):
        text = up if ("??" not in up and up) else low
        resolved[addr] = text if text else "?"
    return resolved


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--elf", type=Path, default=REPO / "build" / "lumeos.elf")
    parser.add_argument("--qemu", default="qemu-system-arm")
    parser.add_argument("--machine", default="raspi0")
    parser.add_argument("--strategy", default="bios",
                        choices=("bios", "loader", "kernel"))
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--blocks", type=int, default=12,
                        help="how many trailing executed blocks to symbolise")
    args = parser.parse_args(argv)

    qemu = shutil.which(args.qemu) or (args.qemu if Path(args.qemu).exists() else None)
    if qemu is None:
        print(f"DIAG: qemu-system-arm not found ({args.qemu})")
        return 0
    if not args.image.is_file():
        print(f"DIAG: kernel image {args.image} does not exist")
        return 1

    base = [qemu, "-M", args.machine, "-display", "none", "-monitor", "none",
            "-serial", "stdio", "-rtc", "base=utc"]
    if args.strategy == "bios":
        argv_qemu = base + ["-bios", str(args.image)]
    elif args.strategy == "loader":
        argv_qemu = base + ["-device",
                            f"loader,file={args.image},addr=0x8000,cpu-num=0"]
    else:
        argv_qemu = base + ["-kernel", str(args.image)]

    with tempfile.TemporaryDirectory(prefix="lume-diag-") as tmp:
        log_path = Path(tmp) / "qemu-debug.log"
        argv_qemu += ["-d", "in_asm,int,unimp,guest_errors,cpu_reset",
                      "-D", str(log_path)]

        serial = ""
        try:
            proc = subprocess.run(argv_qemu, stdin=subprocess.DEVNULL,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  timeout=args.timeout, text=True, errors="replace")
            serial = proc.stdout or ""
            exit_note = f"QEMU exited with status {proc.returncode}"
        except subprocess.TimeoutExpired as exc:
            serial = (exc.stdout or "") if isinstance(exc.stdout, str) else \
                     (exc.stdout or b"").decode("utf-8", "replace")
            exit_note = f"QEMU was still running after {args.timeout:g}s"

        print(f"DIAG: strategy '{args.strategy}', {exit_note}")
        print(f"DIAG: serial output: {len(serial)} bytes"
              + (f", first 120: {serial[:120]!r}" if serial else " (nothing printed)"))

        if not log_path.is_file():
            print("DIAG: QEMU wrote no debug log")
            return 0

        lines = log_path.read_text(errors="replace").splitlines()
        print(f"DIAG: qemu debug log: {len(lines)} lines")

        # ---- executed blocks ----
        executed: list[int] = []
        for i, line in enumerate(lines):
            if line.startswith("IN:"):
                for follow in lines[i + 1:i + 3]:
                    match = ADDRESS_RE.match(follow.strip())
                    if match:
                        executed.append(int(match.group(1), 16))
                        break
        print(f"DIAG: translated blocks executed: {len(executed)}")
        if executed:
            tail = executed[-args.blocks:]
            symbols = symbolize(tail, args.elf)
            print(f"DIAG: last {len(tail)} executed blocks (address: symbol):")
            for addr in tail:
                print(f"DIAG:   0x{addr:08x}  {symbols.get(addr, '?')}")
            if len(set(executed[-50:])) <= 2:
                print(f"DIAG: the guest is spinning in {len(set(executed[-50:]))} "
                      "block(s): that address is the hang")
        else:
            print("DIAG: no guest code was translated at all - the CPU never "
                  "started executing at the expected entry point")

        # ---- exceptions ----
        ints = [line for line in lines if line.startswith("Taking exception")
                or "exception" in line.lower() and line.startswith("IN:")]
        take = [line for line in lines if "Taking exception" in line]
        print(f"DIAG: exceptions taken: {len(take)}")
        for line in take[:5]:
            print(f"DIAG:   {line.strip()[:160]}")
        if len(take) > 5:
            print(f"DIAG:   ... and {len(take) - 5} more")

        # ---- unimplemented / unassigned accesses ----
        bad = [line for line in lines
               if "unimp" in line.lower() or "unassigned" in line.lower()]
        print(f"DIAG: unimplemented/unassigned accesses: {len(bad)}")
        for line in bad[:8]:
            print(f"DIAG:   {line.strip()[:160]}")

        # ---- where the log ends ----
        print("DIAG: last 6 log lines:")
        for line in lines[-6:]:
            print(f"DIAG:   {line.strip()[:160]}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
