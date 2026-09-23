"""Tests for tools/embed_user.py.

The tool embeds a userspace ELF in the kernel image, and it validates the parts
of the image the kernel's loader will validate.  Doing that at build time is the
difference between "the kernel refuses to load its init program with a clear
message" and "the board boots to a shell and does not run init", so the checks
are worth testing themselves.

A minimal ELF32 ARM executable is built by hand here rather than shipped as a
fixture: it keeps the test honest about what the tool accepts (the fields that
must be right are the ones written out below) and keeps a binary blob out of the
repository.
"""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import embed_user  # noqa: E402


def make_elf(*, machine: int = 40, etype: int = 2, entry: int = 0x00010000,
             elfclass: int = 1, endian: int = 1, magic: bytes = b"\x7fELF") -> bytes:
    """A minimal but structurally complete ELF32 header + one PT_LOAD."""
    ehdr = bytearray(52)
    ehdr[0:4] = magic
    ehdr[4] = elfclass
    ehdr[5] = endian
    ehdr[6] = 1                      # EI_VERSION
    struct.pack_into("<HH", ehdr, 16, etype, machine)
    struct.pack_into("<I", ehdr, 20, 1)          # e_version
    struct.pack_into("<I", ehdr, 24, entry)      # e_entry
    struct.pack_into("<I", ehdr, 28, 52)         # e_phoff
    struct.pack_into("<I", ehdr, 32, 0)          # e_shoff
    struct.pack_into("<I", ehdr, 36, 0x05000200) # e_flags (EABI5 | hard-float off)
    struct.pack_into("<H", ehdr, 40, 52)         # e_ehsize
    struct.pack_into("<H", ehdr, 42, 32)         # e_phentsize
    struct.pack_into("<H", ehdr, 44, 1)          # e_phnum
    phdr = struct.pack("<IIIIIIII", 1, 0x1000, 0x00010000, 0x00010000,
                       16, 16, 5, 0x1000)
    return bytes(ehdr) + phdr + b"\x00" * 8


class EmbedUserTests(unittest.TestCase):
    def write_and_run(self, image: bytes) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as tmp:
            elf = Path(tmp) / "init.elf"
            out = Path(tmp) / "blob.c"
            elf.write_bytes(image)
            try:
                rc = embed_user.main([str(elf), "--symbol", "test_blob",
                                      "--output", str(out)])
            except SystemExit as exc:      # the tool reports refusals this way
                code = exc.code
                rc = code if isinstance(code, int) else (0 if code is None else 1)
                return rc, str(exc)
            return rc, out.read_text() if out.exists() else ""

    def test_accepts_a_static_arm_executable(self):
        rc, text = self.write_and_run(make_elf())
        self.assertEqual(rc, 0, text)
        self.assertIn("const u8 test_blob[]", text)
        self.assertIn("test_blob_size", text)
        self.assertIn("test_blob_entry", text)
        self.assertIn("0x00010000", text)

    def test_blob_bytes_match_the_input(self):
        image = make_elf()
        rc, text = self.write_and_run(image)
        self.assertEqual(rc, 0)
        body = text.split("= {", 1)[1].split("};", 1)[0]
        emitted = bytes(int(tok.rstrip(","), 16)
                        for tok in body.split()
                        if tok.startswith("0x"))
        self.assertEqual(emitted, image)

    def test_rejects_a_foreign_architecture(self):
        rc, text = self.write_and_run(make_elf(machine=62))   # EM_X86_64
        self.assertNotEqual(rc, 0)
        self.assertIn("expected ARM", text)

    def test_rejects_a_non_executable(self):
        rc, text = self.write_and_run(make_elf(etype=3))      # ET_DYN
        self.assertNotEqual(rc, 0)
        self.assertIn("ET_EXEC", text)

    def test_rejects_a_64_bit_image(self):
        rc, text = self.write_and_run(make_elf(elfclass=2))
        self.assertNotEqual(rc, 0)
        self.assertIn("ELFCLASS32", text)

    def test_rejects_a_non_elf(self):
        rc, text = self.write_and_run(make_elf(magic=b"NOTF"))
        self.assertNotEqual(rc, 0)
        self.assertIn("not an ELF", text)

    def test_rejects_a_truncated_image(self):
        rc, text = self.write_and_run(make_elf()[:20])
        self.assertNotEqual(rc, 0)
        self.assertIn("too small", text)

    def test_rejects_an_entry_in_the_null_page(self):
        rc, text = self.write_and_run(make_elf(entry=0x00000010))
        self.assertNotEqual(rc, 0)
        self.assertIn("null page", text)

    def test_digest_is_of_the_whole_image(self):
        image = make_elf()
        rc, text = self.write_and_run(image)
        self.assertEqual(rc, 0)
        import hashlib
        self.assertIn(hashlib.sha256(image).hexdigest(), text)


if __name__ == "__main__":
    unittest.main()
