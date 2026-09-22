"""Compile the portable kernel sources for the *host* CPU and run their tests.

The kernel is split so that a few translation units do not touch hardware at
all: kernel/kernel/printf.c (the freestanding printf family) and
kernel/kernel/string.c (the string/memory library).  Building them with the
host C compiler and linking them against tests/host/ktest_main.c gives those
two files real coverage on any development machine, with the real kernel
headers, and without needing an ARM board or an emulator.

What this proves: the format engine and the string library behave correctly.
What it does not prove: anything about ARM code generation, MMU, interrupts or
drivers -- those need tests/qemu (emulated) or a real Pi Zero W.

The test is skipped (not failed) when no host C compiler is installed, so that
`make test-host` still works on a stripped-down system.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
KERNEL_INCLUDE = REPO / "kernel" / "include"

SOURCES = [
    REPO / "tests" / "host" / "ktest_main.c",
    REPO / "kernel" / "kernel" / "printf.c",
    REPO / "kernel" / "kernel" / "string.c",
    # The division cores behind the ARM EABI helpers.  The assembly thunks
    # that give them their rtabi register layout are ARM-only and therefore
    # exercised by the in-kernel self tests under QEMU, not here.
    REPO / "kernel" / "kernel" / "divmod.c",
]

HOST_CFLAGS = [
    "-std=gnu11",
    "-O1",
    "-g",
    # The kernel's string functions are ordinary functions, but the compiler
    # knows libc's versions as builtins and would replace the calls with
    # inline sequences, which would test the compiler instead of LumeOS.
    "-fno-builtin",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    f"-I{KERNEL_INCLUDE}",
]


def find_host_cc() -> str | None:
    for candidate in (os.environ.get("HOSTCC"), "cc", "gcc", "clang"):
        if candidate and shutil.which(candidate):
            return candidate
    return None


class KernelLibTests(unittest.TestCase):
    def test_printf_and_string_library(self):
        cc = find_host_cc()
        if cc is None:
            self.skipTest("no host C compiler found (set HOSTCC to override)")

        with tempfile.TemporaryDirectory(prefix="lume-hosttest-") as tmp:
            binary = Path(tmp) / "ktest"
            compile_cmd = [cc, *HOST_CFLAGS, *[str(s) for s in SOURCES], "-o", str(binary)]
            result = subprocess.run(compile_cmd, cwd=REPO, capture_output=True, text=True)

            self.assertEqual(
                result.returncode, 0,
                "host compilation of the kernel library failed:\n"
                f"$ {' '.join(compile_cmd)}\n{result.stdout}{result.stderr}",
            )
            self.assertNotIn("warning:", result.stderr,
                             f"host build produced warnings:\n{result.stderr}")

            run = subprocess.run([str(binary)], capture_output=True, text=True)
            # The test binary prints a one line summary; echo it into the test
            # log so a passing run still shows how many checks executed.
            print(run.stdout.strip())
            self.assertEqual(run.returncode, 0,
                             f"kernel library tests failed:\n{run.stdout}{run.stderr}")
            self.assertIn(" 0 failures", run.stdout)


if __name__ == "__main__":
    unittest.main()
