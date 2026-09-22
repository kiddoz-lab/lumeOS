#!/usr/bin/env python3
"""Boot the LumeOS kernel image under QEMU and verify that it comes up.

Scope, stated plainly: this is an *emulator* test.  A pass here means the
kernel image boots, runs its in-kernel self tests and reaches the shell *on
QEMU's bcm2835 model*.  It is not evidence about a real Raspberry Pi Zero W:
the SoC peripherals, the firmware, the SD card and the boot firmware chain are
all simulated by QEMU, and several of them differ from the real hardware
(see docs/testing.md).

How the kernel gets into memory
-------------------------------
On a real board, the VideoCore firmware loads kernel.img at physical 0x8000 and
jumps to its first byte.  QEMU offers several ways to emulate that, and they do
not agree on the load address:

  * ``-bios``      the raspi machines load this file at 0x8000 and set the
                   entry point to 0x8000 -- the closest match to the firmware;
  * ``-device loader,file=...,addr=0x8000,cpu-num=0``
                   loads the file at an explicit address and starts the CPU
                   there;
  * ``-kernel``    QEMU's "direct Linux kernel" path, which treats a raw
                   binary as a Linux kernel, puts it at 0x10000 and enters it
                   through a Linux boot stub.

Because LumeOS is linked for load address 0x8000, the script tries the
strategies in the order above and reports which one produced a boot.  The
strategies only differ in *how the emulator loads a bare-metal image*; every
strategy is checked against the same boot markers.

Usage:
    tests/qemu/run_qemu_test.py --image build/kernel.img
    tests/qemu/run_qemu_test.py --image build/kernel.img --qemu /usr/bin/qemu-system-arm

Exit status: 0 when the kernel boots, 1 when it does not, and 0 with a SKIP
message when qemu-system-arm is not installed (use --required to make that a
failure instead).
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Strings the kernel prints during a successful boot.  They come from
# kernel/kernel/main.c and kernel/kernel/selftest.c.
BOOT_MARKERS = [
    "LumeOS 0.1.0 (armv6kz)",
    "selftest: running kernel self tests",
    "LumeOS: boot complete",
    "main: entering the idle loop",
]

# "selftest: 24/24 checks passed"
SELFTEST_RE = re.compile(r"selftest: (\d+)/(\d+) checks passed")
# The kernel prints "selftest: N/M checks passed" on every boot; a run that
# reports fewer checks than this has lost most of its self tests (a build
# problem, a truncated boot), and the count is asserted so that the summary
# cannot quietly become vacuous.  The kernel currently defines 44 checks.
MIN_SELFTEST_CHECKS = 40

FAILURE_PATTERNS = [
    "PANIC",
    "checks FAILED",
    "FAIL ",
    "Oops",
]


def qemu_argv(qemu: str, machine: str, strategy: str, image: Path) -> list[str]:
    base = [qemu, "-M", machine, "-display", "none", "-monitor", "none",
            "-serial", "stdio", "-rtc", "base=utc"]
    if strategy == "bios":
        return base + ["-bios", str(image)]
    if strategy == "loader":
        return base + ["-device", f"loader,file={image},addr=0x8000,cpu-num=0"]
    if strategy == "kernel":
        return base + ["-kernel", str(image)]
    raise ValueError(strategy)


STRATEGIES = ("bios", "loader", "kernel")


def run_once(qemu: str, machine: str, strategy: str, image: Path,
             timeout: float) -> tuple[str, int, str]:
    """Run QEMU until the timeout (a booted LumeOS never exits) and capture output."""
    argv = qemu_argv(qemu, machine, strategy, image)
    try:
        proc = subprocess.run(argv, stdin=subprocess.DEVNULL,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              timeout=timeout, text=True, errors="replace")
        return "exited", proc.returncode, proc.stdout
    except subprocess.TimeoutExpired as exc:
        output = exc.stdout or ""
        if isinstance(output, bytes):
            output = output.decode("utf-8", "replace")
        # Timeout is the expected outcome for an OS that never powers off.
        return "timeout", 0, output



def qemu_memory_tree(qemu: str, machine: str) -> list[str]:
    """Ask QEMU where it mapped the peripherals (``info mtree`` via the monitor).

    This answers the question that decides whether a kernel can talk to the
    UART at all: is the BCM2835 peripheral block visible at the address the
    kernel's memory map assumes (0x20000000 on a Pi 1)?
    """
    argv = [qemu, "-M", machine, "-display", "none", "-serial", "none",
            "-monitor", "stdio", "-S"]
    try:
        proc = subprocess.run(argv, input="info mtree\nquit\n",
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, errors="replace", timeout=30)
    except (subprocess.TimeoutExpired, OSError) as exc:
        return [f"(could not read the QEMU memory tree: {exc})"]

    interesting = []
    for line in proc.stdout.splitlines():
        low = line.lower()
        if ("bcm2835" in low or "uart" in low or "peripheral" in low
                or "system" in low or "ram" == low.strip()):
            interesting.append(line.rstrip())
    return interesting[:60]


def qemu_debug_pass(qemu: str, machine: str, strategy: str, image: Path,
                    timeout: float, log_path: Path) -> list[str]:
    """Run one boot attempt with QEMU's own diagnostics enabled.

    ``-d int,unimp,guest_errors,cpu_reset`` makes QEMU report exceptions taken
    by the guest, accesses to unimplemented or unassigned addresses and CPU
    resets.  If the kernel dies early, this is where the reason shows up.
    """
    argv = qemu_argv(qemu, machine, strategy, image) + [
        "-d", "int,unimp,guest_errors,cpu_reset", "-D", str(log_path)]
    try:
        subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=timeout, text=True)
    except subprocess.TimeoutExpired:
        pass
    except OSError as exc:
        return [f"(could not start the QEMU debug pass: {exc})"]

    if not log_path.is_file():
        return ["(QEMU wrote no debug log)"]

    lines = [line for line in log_path.read_text(errors="replace").splitlines()
             if line.strip()]
    report = [f"QEMU debug log: {log_path} ({len(lines)} lines)"]

    hits = [line for line in lines
            if ("unimp" in line.lower() or "unassigned" in line.lower()
                or "invalid" in line.lower() or "guest error" in line.lower())]
    if hits:
        report.append(f"unimplemented/unassigned accesses ({len(hits)} lines), first 20:")
        report.extend(f"  {line}" for line in hits[:20])
    else:
        report.append("no unimplemented/unassigned accesses logged")

    resets = [line for line in lines if "reset" in line.lower()]
    if resets:
        report.append("CPU reset events (first 5):")
        report.extend(f"  {line}" for line in resets[:5])

    report.append("last 15 lines of the emulator log:")
    report.extend(f"  {line}" for line in lines[-15:])
    return report


def evaluate(output: str) -> tuple[bool, list[str]]:
    problems: list[str] = []

    for marker in BOOT_MARKERS:
        if marker not in output:
            problems.append(f"missing boot marker: {marker!r}")

    for pattern in FAILURE_PATTERNS:
        if pattern in output:
            problems.append(f"output contains failure pattern: {pattern!r}")

    match = SELFTEST_RE.search(output)
    if not match:
        problems.append("no 'selftest: N/M checks passed' summary line")
    else:
        passed, total = int(match.group(1)), int(match.group(2))
        if passed != total:
            problems.append(f"in-kernel self tests failed: {passed}/{total} passed")
        if total < MIN_SELFTEST_CHECKS:
            problems.append(f"only {total} self test checks ran (expected at least "
                            f"{MIN_SELFTEST_CHECKS})")

    return (not problems), problems


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--image", type=Path, required=True,
                        help="flat kernel image (build/kernel.img)")
    parser.add_argument("--elf", type=Path, default=None,
                        help="linked kernel ELF; used only for the log line")
    parser.add_argument("--qemu", default="qemu-system-arm")
    parser.add_argument("--machine", default="raspi0")
    parser.add_argument("--timeout", type=float, default=60.0,
                        help="seconds to let the kernel run before checking output")
    parser.add_argument("--no-diagnostics", action="store_true",
                        help="skip the extra QEMU runs that explain a failure")
    parser.add_argument("--diagnose-timeout", type=float, default=10.0,
                        help="seconds per diagnostic QEMU run")
    parser.add_argument("--required", action="store_true",
                        help="fail (instead of skipping) when QEMU is not installed")
    args = parser.parse_args(argv)

    qemu = shutil.which(args.qemu)
    if qemu is None and os.path.exists(args.qemu):
        qemu = args.qemu
    if qemu is None:
        message = (f"qemu-system-arm not found ({args.qemu}); skipping the emulator "
                   "boot test.  Install qemu-system-arm to run it locally.")
        if args.required or os.environ.get("CI"):
            print(f"run_qemu_test: ERROR: {message}", file=sys.stderr)
            return 1
        print(f"run_qemu_test: SKIP: {message}", file=sys.stderr)
        return 0

    if not args.image.is_file():
        print(f"run_qemu_test: ERROR: kernel image {args.image} does not exist",
              file=sys.stderr)
        return 1

    version = subprocess.run([qemu, "--version"], capture_output=True, text=True)
    print(f"run_qemu_test: {version.stdout.strip().splitlines()[0]}")
    print(f"run_qemu_test: image {args.image} "
          f"({args.image.stat().st_size} bytes), machine {args.machine}")

    failures: list[str] = []
    # (strategy, last few console lines) - reprinted in the final report, which
    # is the last thing this program writes to stdout.  That matters because CI
    # lifts the tail of this log into an annotation and stdout is block
    # buffered, so anything printed before the diagnostics block may as well not
    # exist.
    console_tails: list[tuple[str, list[str]]] = []
    selftest_lines: list[str] = []

    for strategy in STRATEGIES:
        print(f"run_qemu_test: strategy '{strategy}': "
              f"{' '.join(qemu_argv(qemu, args.machine, strategy, args.image))}")
        how, code, output = run_once(qemu, args.machine, strategy, args.image,
                                     args.timeout)
        ok, problems = evaluate(output)

        if ok:
            match = SELFTEST_RE.search(output)
            print(f"run_qemu_test: PASS via '{strategy}' "
                  f"(kernel self tests {match.group(1)}/{match.group(2)}, "
                  f"booting in QEMU's {args.machine} model)")
            print("run_qemu_test: --- kernel console output ---")
            print(output.rstrip())
            print("run_qemu_test: --- end of console output ---")
            print("run_qemu_test: NOTE: emulator result only; a real Raspberry Pi "
                  "Zero W boot is still unverified (see docs/testing.md).")
            return 0

        if how == "exited" and code == 0 and not output.strip():
            reason = "QEMU exited immediately without any guest output"
        elif how == "exited":
            reason = f"QEMU exited early (status {code})"
        else:
            reason = f"no boot within {args.timeout:g}s"
        print(f"run_qemu_test: strategy '{strategy}' failed: {reason}")
        for problem in problems:
            print(f"run_qemu_test:   - {problem}")
        failures.append(f"{strategy}: {reason}; " + "; ".join(problems))
        if output.strip():
            console_tails.append((strategy, output.strip().splitlines()[-4:]))
            for line in output.splitlines():
                if "selftest:" in line and ("FAIL" in line or "passed" in line
                                            or "checks" in line):
                    entry = line.strip()
                    if entry not in selftest_lines:
                        selftest_lines.append(entry)

        if output.strip():
            tail = output.strip().splitlines()[-15:]
            print("run_qemu_test:   last output:")
            for line in tail:
                print(f"run_qemu_test:     | {line}")

    if not args.no_diagnostics:
        print("run_qemu_test: diagnostics: asking QEMU where the peripherals are")
        for line in qemu_memory_tree(qemu, args.machine):
            print(f"run_qemu_test: mtree: {line}")

        print("run_qemu_test: diagnostics: booting the bios strategy with "
              "QEMU's own debug log enabled")
        log_path = args.image.parent / "qemu-debug.log"
        for line in qemu_debug_pass(qemu, args.machine, STRATEGIES[0], args.image,
                                    args.diagnose_timeout, log_path):
            print(f"run_qemu_test: qemu-debug: {line}")

    # ---- final report, deliberately the last thing on stdout ----
    print("run_qemu_test: ---- failure report ----")
    print("run_qemu_test: RESULT: no QEMU loading strategy booted the kernel to "
          "all of its markers")
    for failure in failures:
        print(f"run_qemu_test: failed: {failure}")
    if selftest_lines:
        print("run_qemu_test: in-kernel self tests reported:")
        for line in selftest_lines[:12]:
            print(f"run_qemu_test:   {line[:150]}")
    for strategy, tail in console_tails:
        print(f"run_qemu_test: console tail ({strategy}):")
        for line in tail:
            print(f"run_qemu_test:   | {line}")

    print("run_qemu_test: ERROR: no QEMU loading strategy booted the kernel",
          file=sys.stderr)
    for failure in failures:
        print(f"run_qemu_test:   {failure}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
