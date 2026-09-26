"""Compile and run the initial-stack layout test for the *host* CPU.

The stack builder (kernel/kernel/ustack.c) is the code that decides where argc,
argv, envp and the auxiliary vector go in a new program's stack.  Getting it
wrong is not a crash in the kernel - it is a crash in a program that is already
in user mode, with nothing but a data abort to explain it.  So it gets a test
that runs anywhere: tests/host/ustack_main.c supplies a fake memory model and
the four kernel functions the builder calls, and this test compiles them
together with the builder and runs it.

What this proves: the layout, the alignment, the terminators, the auxv contents
and the refusal paths are right for the inputs given.  What it does not prove:
the page tables are real, or that a program actually reads the stack that way -
the in-kernel self test covers the first under QEMU and init's own checks cover
the second on a real boot.

Skipped (not failed) when no host C compiler is installed.
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
    REPO / "tests" / "host" / "ustack_main.c",
    REPO / "kernel" / "kernel" / "ustack.c",
    REPO / "kernel" / "kernel" / "random.c",
]

HOST_CFLAGS = [
    "-std=gnu11",
    "-O1",
    "-g",
    "-fno-builtin",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    # The kernel is 32-bit: there, u32 and a pointer are the same width and the
    # code casts between them freely (the rest of the kernel does too).  On a
    # 64-bit host that is a warning about the *host* ABI rather than about the
    # code under test, and the alternative - making the test compile the kernel
    # for a 32-bit host - would need a multilib toolchain that often is not
    # installed.  So the two casts are silenced; everything else stays on.
    "-Wno-int-to-pointer-cast",
    "-Wno-pointer-to-int-cast",
    f"-I{KERNEL_INCLUDE}",
]


def find_host_cc() -> str | None:
    for candidate in (os.environ.get("HOSTCC"), "cc", "gcc", "clang"):
        if candidate and shutil.which(candidate):
            return candidate
    return None


class UstackLayoutTests(unittest.TestCase):
    def test_initial_stack_layout(self):
        cc = find_host_cc()
        if cc is None:
            self.skipTest("no host C compiler found (set HOSTCC to override)")

        with tempfile.TemporaryDirectory(prefix="lume-ustack-") as tmp:
            binary = Path(tmp) / "ustack-test"
            cmd = [cc, *HOST_CFLAGS, *[str(s) for s in SOURCES], "-o", str(binary)]
            result = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)

            self.assertEqual(
                result.returncode, 0,
                "host compilation of the stack builder failed:\n"
                f"$ {' '.join(cmd)}\n{result.stdout}{result.stderr}",
            )
            self.assertNotIn("warning:", result.stderr,
                             f"host build produced warnings:\n{result.stderr}")

            run = subprocess.run([str(binary)], capture_output=True, text=True)
            print(run.stdout.strip())
            self.assertEqual(run.returncode, 0,
                             f"stack layout checks failed:\n{run.stdout}{run.stderr}")
            self.assertIn(" 0 failures", run.stdout)


if __name__ == "__main__":
    unittest.main()
