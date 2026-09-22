"""Host-side unit tests for the LumeOS build tools.

Run with:  make test-host      (which is `python3 -m unittest discover -s tests/host`)

These tests exercise the parts of an ARM operating system that can be verified
without an ARM CPU: the FAT16 image builder, the ELF-to-flat-binary converter
and the instruction-set checker.  Everything that touches hardware is covered
by the in-kernel self tests and by tests/qemu/run_qemu_test.py.
"""

from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

import check_isa  # noqa: E402
import elf2bin  # noqa: E402
import mkimage  # noqa: E402
import verify_image  # noqa: E402


class Fat16Tests(unittest.TestCase):
    """The SD card image must be a valid FAT16 volume the firmware can read."""

    FILES = {
        "kernel.img": b"LUME" * 700,
        "config.txt": b"kernel=kernel.img\n",
    }

    @classmethod
    def setUpClass(cls):
        cls.image = mkimage.build_image(dict(cls.FILES), size_mib=8)

    def test_mbr(self):
        mbr = self.image[:512]
        self.assertEqual(mbr[510:512], b"\x55\xAA")
        entry = mbr[446:462]
        self.assertEqual(entry[0], 0x80)                     # bootable
        self.assertEqual(entry[4], mkimage.FAT16_TYPE)
        start, count = struct.unpack_from("<II", entry, 8)
        self.assertEqual(start, mkimage.PARTITION_START)
        self.assertEqual((start + count) * 512, len(self.image))

    def test_bpb(self):
        fat = self.image[mkimage.PARTITION_START * 512:]
        self.assertEqual(fat[0:3], b"\xEB\x3C\x90")
        bps, spc, reserved, fats, root_entries = struct.unpack_from("<HBHBH", fat, 11)
        self.assertEqual(bps, 512)
        self.assertEqual(reserved, 1)
        self.assertEqual(fats, 2)
        self.assertEqual(root_entries, 512)
        self.assertTrue(1 <= spc <= 128)
        self.assertEqual(fat[54:62], b"FAT16   ")
        volume_label = fat[43:54].decode("ascii")
        self.assertEqual(volume_label, "LUMEOS     ")

    def test_root_directory_and_data(self):
        fat = self.image[mkimage.PARTITION_START * 512:]
        reserved = struct.unpack_from("<H", fat, 14)[0]
        num_fats = struct.unpack_from("<B", fat, 16)[0]
        root_entries = struct.unpack_from("<H", fat, 17)[0]
        bps = struct.unpack_from("<H", fat, 11)[0]
        fat_size = struct.unpack_from("<H", fat, 22)[0]
        root_sectors = (root_entries * 32 + bps - 1) // bps
        root_start = (reserved + num_fats * fat_size) * 512
        entries = {}
        for i in range(512):
            entry = fat[root_start + i * 32:root_start + (i + 1) * 32]
            if entry[0] == 0x00:
                break
            if entry[0] == 0xE5:
                continue
            name = entry[0:8].decode("ascii").rstrip() + "." + entry[8:11].decode("ascii").rstrip(".")
            first_cluster = struct.unpack_from("<H", entry, 26)[0]
            size = struct.unpack_from("<I", entry, 28)[0]
            entries[name.rstrip(".")] = (first_cluster, size)

        self.assertIn("KERNEL.IMG", entries)
        self.assertIn("CONFIG.TXT", entries)
        for name, (cluster, size) in entries.items():
            data = self._read_chain(fat, cluster, size, spc=struct.unpack_from("<B", fat, 13)[0],
                                    data_start=(reserved + num_fats * fat_size + root_sectors) * 512)
            expected = self.FILES[name.lower()]
            self.assertEqual(data, expected, f"{name} content mismatch")
            self.assertEqual(size, len(expected))

    def _read_chain(self, fat, cluster, size, spc, data_start):
        fat_start = 512                    # reserved sector 0 is the BPB itself
        out = bytearray()
        seen = set()
        while cluster and cluster < 0xFFF8:
            self.assertNotIn(cluster, seen, "FAT loop detected")
            seen.add(cluster)
            offset = data_start + (cluster - 2) * spc * 512
            out += fat[offset:offset + spc * 512]
            cluster = struct.unpack_from("<H", fat, fat_start + cluster * 2)[0]
        return bytes(out[:size])

    def test_round_trip_through_verify_image(self):
        """The verifier must accept an image this builder produced."""
        with tempfile.TemporaryDirectory(prefix="lume-verify-") as tmp:
            image_path = Path(tmp) / "sd.img"
            kernel_path = Path(tmp) / "kernel.img"
            image_path.write_bytes(self.image)
            kernel_path.write_bytes(self.FILES["kernel.img"])

            files = dict(self.FILES)
            files["bootcode.bin"] = b"\x00" * 4096
            files["start.elf"] = b"\x01" * 8192
            files["fixup.dat"] = b"\x02" * 1024
            firmware = Path(tmp) / "firmware"
            firmware.mkdir()
            for name, data in files.items():
                (firmware / name).write_bytes(data)

            full = mkimage.build_image(files, size_mib=8)
            image_path.write_bytes(full)
            kernel_path.write_bytes(files["kernel.img"])

            details = verify_image.verify(image_path, kernel_path, firmware)
            self.assertTrue(details["kernel_matches_build"])
            self.assertEqual(details["partition_start"], mkimage.PARTITION_START)
            self.assertEqual(details["label"], "LUMEOS")
            for name in ("KERNEL.IMG", "START.ELF", "BOOTCODE.BIN", "FIXUP.DAT"):
                self.assertIn(name, details["files"])

    def test_verifier_rejects_a_corrupted_file(self):
        """Corrupt one byte of KERNEL.IMG's data: the FAT stays valid, so only
        the content comparison can catch it."""
        files = dict(self.FILES)
        files["bootcode.bin"] = b"\x00" * 4096
        files["start.elf"] = b"\x01" * 8192
        files["fixup.dat"] = b"\x02" * 1024
        image = bytearray(mkimage.build_image(files, size_mib=8))

        # Locate KERNEL.IMG's first cluster through the FAT structures.
        partition = image[mkimage.PARTITION_START * 512:]
        bpb = verify_image.parse_bpb(partition)
        root = verify_image.read_root_directory(partition, bpb)
        first_cluster, _size = root["KERNEL.IMG"]
        data_start = ((bpb["reserved"] + bpb["num_fats"] * bpb["fat_size"]
                       + bpb["root_dir_sectors"]) * bpb["bytes_per_sector"])
        offset = mkimage.PARTITION_START * 512 + data_start + \
            (first_cluster - 2) * bpb["sectors_per_cluster"] * bpb["bytes_per_sector"]
        image[offset] ^= 0xFF

        with tempfile.TemporaryDirectory(prefix="lume-verify-") as tmp:
            image_path = Path(tmp) / "sd.img"
            image_path.write_bytes(bytes(image))
            kernel_path = Path(tmp) / "kernel.img"
            kernel_path.write_bytes(files["kernel.img"])

            with self.assertRaises(verify_image.VerifyError) as ctx:
                verify_image.verify(image_path, kernel_path, None)
            self.assertIn("KERNEL.IMG", str(ctx.exception))

    def test_refuses_tiny_image(self):
        with self.assertRaises(mkimage.ImageError):
            mkimage.build_image(dict(self.FILES), size_mib=1)

    def test_requires_firmware_files(self):
        with self.assertRaises(mkimage.ImageError):
            mkimage.collect_boot_files(Path("/nonexistent-firmware"),
                                       Path(__file__), [], None, None)


class CheckIsaTests(unittest.TestCase):
    """The ISA gate is the only thing keeping ARMv7 code out of the kernel."""

    def test_accepts_the_armv6_build_attributes(self):
        # GNU readelf synthesises a name from Tag_CPU_arch for a linked binary;
        # llvm-readelf keeps the compiler's longer name.  Both are ARMv6KZ.
        for attrs in (
            {"cpu_arch": "v6KZ", "cpu_name": "6KZ"},
            {"cpu_arch": "v6", "cpu_name": "arm1176jzf-s"},
            {"cpu_arch": "v6KZ"},                              # name omitted
            {"cpu_arch": "6K"},                                # v6K also OK
        ):
            self.assertEqual(check_isa.attribute_problems(attrs), [], attrs)

    def test_rejects_the_wrong_architecture(self):
        for arch in ("v7", "v8", "v6-M"):
            with self.subTest(arch=arch):
                problems = check_isa.attribute_problems({"cpu_arch": arch})
                self.assertEqual(len(problems), 1)
                self.assertIn("Tag_CPU_arch", problems[0])

    def test_rejects_a_newer_core_name(self):
        problems = check_isa.attribute_problems({"cpu_arch": "v6KZ",
                                                 "cpu_name": "Cortex-A9"})
        self.assertEqual(len(problems), 1)
        self.assertIn("Cortex-A9", problems[0])

    def test_instruction_scan(self):
        allowed = [(0x0, "add"), (0x4, "ldr"), (0x8, "cpsid"), (0xc, "mcr")]
        self.assertEqual(check_isa.check_instructions(allowed), [])

        for mnemonic in ("movw", "sdiv", "vadd.f32", "vpush"):
            with self.subTest(mnemonic=mnemonic):
                problems = check_isa.check_instructions([(0x0, mnemonic)])
                self.assertTrue(problems, f"{mnemonic} must be rejected")

    def test_barrier_hint_is_reported(self):
        problems = check_isa.check_instructions([(0x0, "dmb")])
        self.assertEqual(len(problems), 1)
        self.assertIn("ARMv7 hint", problems[0])


class Elf2BinTests(unittest.TestCase):
    """The flat kernel image must start exactly at the firmware load address."""

    def make_elf(self, paddr: int = 0x8000, vaddr: int = 0xC0008000,
                 payload: bytes = b"\x01\x00\xa0\xe1", extra_phdr: bool = False) -> bytes:
        """Build a minimal 32-bit ARM ELF with one (or two) PT_LOAD segments."""
        ehsize, phentsize = 52, 32
        phoff = ehsize
        phnum = 2 if extra_phdr else 1
        code_off = phoff + phnum * phentsize
        header = struct.pack("<4s5B7x", b"\x7fELF", 1, 1, 1, 0, 0)
        header += struct.pack("<HHIIIIIHHHHHH",
                              2,              # ET_EXEC
                              40,             # EM_ARM
                              1,
                              vaddr,          # entry
                              phoff, 0, 0,
                              ehsize,
                              phentsize, phnum,
                              0, 0, 0)
        phdr = struct.pack("<IIIIIIII", 1, code_off, vaddr, paddr, len(payload),
                           len(payload) + 16, 5, 0x1000)
        if extra_phdr:
            phdr += struct.pack("<IIIIIIII", 1, code_off, 0xC0000000, 0xC0000000,
                                len(payload), len(payload), 5, 0x1000)
        return header + phdr + payload

    def test_ok(self):
        elf = self.make_elf()
        segs = elf2bin.parse_elf(elf)
        self.assertEqual(len(segs), 1)
        image = elf2bin.build_image(segs, 0x8000)
        self.assertEqual(len(image), 4 + 16)      # filesz + bss tail
        self.assertEqual(image[:4], b"\x01\x00\xa0\xe1")

    def test_wrong_load_address(self):
        segs = elf2bin.parse_elf(self.make_elf(paddr=0x10000))
        with self.assertRaises(elf2bin.ElfError) as ctx:
            elf2bin.build_image(segs, 0x8000)
        self.assertIn("load address", str(ctx.exception))

    def test_rejects_virtual_only_segment(self):
        # This is the failure mode a linker script without PHDRS produces.
        segs = elf2bin.parse_elf(self.make_elf(extra_phdr=True))
        with self.assertRaises(elf2bin.ElfError) as ctx:
            elf2bin.build_image(segs, 0x8000)
        self.assertIn("physical address", str(ctx.exception))

    def test_rejects_vector_page_overlap(self):
        big = b"\x00" * (elf2bin.VECTOR_PAGE_PA - 0x8000 + 16)
        segs = elf2bin.parse_elf(self.make_elf(payload=big))
        with self.assertRaises(elf2bin.ElfError) as ctx:
            elf2bin.build_image(segs, 0x8000)
        self.assertIn("vector page", str(ctx.exception))

    def test_rejects_non_arm(self):
        elf = bytearray(self.make_elf())
        struct.pack_into("<H", elf, 18, 62)       # EM_X86_64
        with self.assertRaises(elf2bin.ElfError):
            elf2bin.parse_elf(bytes(elf))


if __name__ == "__main__":
    unittest.main()
