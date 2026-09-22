#!/usr/bin/env python3
"""Build a bootable Raspberry Pi SD card image for LumeOS.

Creating the image in pure Python keeps the build free of extra dependencies
(mtools, dosfstools, sfdisk): the only thing required is the Raspberry Pi
boot firmware, which the user either copies from an existing Raspberry Pi OS
card or downloads with tools/fetch-firmware.sh.

Layout produced:

    sector 0        master boot record (MBR) with one partition entry
    sector 2048     FAT16 partition (type 0x06) containing
                      BOOTCODE.BIN  START.ELF  FIXUP.DAT  KERNEL.IMG
                      CONFIG.TXT    CMDLINE.TXT  (+ optional extra files)

The FAT16 implementation is deliberately minimal but complete enough for the
VideoCore bootloader: fixed 8.3 names, one cluster chain per file, no
subdirectories.  It is unit tested by tests/host/test_mkimage.py, which parses
the image back and verifies the directory entries and file contents.
"""

from __future__ import annotations

import argparse
import struct
import sys
import time
from pathlib import Path

SECTOR = 512
PARTITION_START = 2048           # 1 MiB alignment, the conventional choice
DEFAULT_SIZE_MIB = 64
FAT16_TYPE = 0x06
FS_TYPE_MIB = 1                  # partition type hint byte in the BPB

# Files the Raspberry Pi firmware needs on the boot partition.  kernel.img is
# produced by this build; the rest come from the firmware release.
REQUIRED_BOOT_FILES = (
    "bootcode.bin",
    "start.elf",
    "fixup.dat",
    "kernel.img",
)


class ImageError(Exception):
    pass


def short_name(name: str) -> bytes:
    """Convert a filename to an 8.3 directory entry name (11 bytes)."""
    name = name.upper()
    if "." in name:
        base, _, ext = name.rpartition(".")
    else:
        base, ext = name, ""
    if len(base) > 8 or len(ext) > 3:
        raise ImageError(f"'{name}' does not fit the 8.3 naming scheme")
    if not base:
        raise ImageError(f"'{name}' has an empty base name")
    return base.ljust(8).encode("ascii") + ext.ljust(3).encode("ascii")


def _fat_time_date(ts: float) -> tuple[int, int]:
    t = time.localtime(ts)
    date = ((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
    time_word = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    return date, time_word


def _compute_fat16_geometry(total_sectors: int, sectors_per_cluster: int = 4) -> dict:
    """Standard FAT16 size computation (Microsoft FAT specification)."""
    reserved = 1
    num_fats = 2
    root_entries = 512
    root_dir_sectors = ((root_entries * 32) + (SECTOR - 1)) // SECTOR

    fat_size = 1
    while True:
        data_sectors = total_sectors - (reserved + num_fats * fat_size + root_dir_sectors)
        clusters = data_sectors // sectors_per_cluster
        needed = ((clusters + 2) * 2 + (SECTOR - 1)) // SECTOR
        if needed == fat_size:
            break
        fat_size = needed
    clusters = (total_sectors - (reserved + num_fats * fat_size + root_dir_sectors)) // sectors_per_cluster
    if clusters > 0xFFF5:
        raise ImageError("partition is too large for FAT16; use a smaller image")
    return {
        "bytes_per_sector": SECTOR,
        "sectors_per_cluster": sectors_per_cluster,
        "reserved_sectors": reserved,
        "num_fats": num_fats,
        "root_entries": root_entries,
        "total_sectors": total_sectors,
        "fat_size": fat_size,
        "root_dir_sectors": root_dir_sectors,
        "clusters": clusters,
    }


def build_fat16(files: dict[str, bytes], total_sectors: int, label: str = "LUMEOS") -> bytes:
    geo = _compute_fat16_geometry(total_sectors)
    image = bytearray(total_sectors * SECTOR)

    fat_start = geo["reserved_sectors"]
    root_start = fat_start + geo["num_fats"] * geo["fat_size"]
    data_start = root_start + geo["root_dir_sectors"]
    spc = geo["sectors_per_cluster"]

    # ---- boot sector (BPB) ----
    oem = b"LUMEOS  "
    boot = bytearray(SECTOR)
    boot[0:3] = b"\xEB\x3C\x90"
    boot[3:11] = oem
    struct.pack_into("<HBHBHHBHHHII", boot, 11,
                     SECTOR, spc, geo["reserved_sectors"], geo["num_fats"],
                     geo["root_entries"], geo["total_sectors"] if geo["total_sectors"] < 0x10000 else 0,
                     0xF8, geo["fat_size"], 0, 0, 0, 0)
    # The struct above covers offsets 11..35; fix the fields that need explicit
    # values because of the 16/32-bit split of the FAT16 BPB.
    struct.pack_into("<H", boot, 19, geo["total_sectors"] if geo["total_sectors"] < 0x10000 else 0)
    struct.pack_into("<H", boot, 22, geo["fat_size"])
    struct.pack_into("<I", boot, 28, 0)                     # hidden sectors
    struct.pack_into("<I", boot, 32,
                     geo["total_sectors"] if geo["total_sectors"] >= 0x10000 else 0)
    struct.pack_into("<HB", boot, 36, 0x80, 0x00)           # drive number, reserved
    boot[38] = 0x29                                          # extended boot signature
    struct.pack_into("<I", boot, 39, int(time.time()))
    boot[43:54] = label.upper().ljust(11)[:11].encode("ascii")
    boot[54:62] = b"FAT16   "
    boot[510:512] = b"\x55\xAA"

    # ---- FATs ----
    fat = [0] * (geo["fat_size"] * SECTOR // 2)
    fat[0] = 0xFFF8
    fat[1] = 0xFFFF

    # ---- root directory + file data ----
    root_entry = 0
    next_cluster = 2
    date, time_word = _fat_time_date(time.time())

    for name in sorted(files):
        data = files[name]
        entries = short_name(name)
        first_cluster = next_cluster
        if data:
            cluster_count = (len(data) + spc * SECTOR - 1) // (spc * SECTOR)
            for i in range(cluster_count):
                cluster = next_cluster + i
                fat[cluster] = cluster + 1 if i < cluster_count - 1 else 0xFFFF
                offset = (data_start + (cluster - 2) * spc) * SECTOR
                chunk = data[i * spc * SECTOR:(i + 1) * spc * SECTOR]
                image[offset:offset + len(chunk)] = chunk
            next_cluster += cluster_count
        else:
            first_cluster = 0

        entry_off = (root_start * SECTOR) + root_entry * 32
        entry = bytearray(32)
        entry[0:11] = entries
        entry[11] = 0x20                       # archive attribute
        struct.pack_into("<H", entry, 14, time_word)
        struct.pack_into("<H", entry, 16, date)
        struct.pack_into("<H", entry, 22, time_word)
        struct.pack_into("<H", entry, 24, date)
        struct.pack_into("<H", entry, 20, first_cluster >> 16)  # high cluster word (0)
        struct.pack_into("<H", entry, 26, first_cluster & 0xFFFF)
        struct.pack_into("<I", entry, 28, len(data))
        image[entry_off:entry_off + 32] = entry
        root_entry += 1

    if root_entry >= geo["root_entries"]:
        raise ImageError("too many files for the FAT16 root directory")

    # ---- write the two FAT copies ----
    fat_bytes = struct.pack(f"<{len(fat)}H", *fat)
    for i in range(geo["num_fats"]):
        start = (fat_start + i * geo["fat_size"]) * SECTOR
        image[start:start + len(fat_bytes)] = fat_bytes

    image[0:SECTOR] = boot
    return bytes(image)


def build_mbr(partition_sectors: int, label: str = "LUMEOS") -> bytes:
    mbr = bytearray(SECTOR)
    # Bootstrap code: a single infinite loop is enough; the VideoCore firmware
    # never executes MBR code, it only reads the partition table.
    mbr[0:4] = b"\xEA\xFE\xFF\xFF"
    entry = bytearray(16)
    entry[0] = 0x80                                       # bootable flag
    entry[1:4] = b"\xFE\xFF\xFF"                          # CHS start (LBA mode)
    entry[4] = FAT16_TYPE
    entry[5:8] = b"\xFE\xFF\xFF"                          # CHS end (LBA mode)
    struct.pack_into("<II", entry, 8, PARTITION_START, partition_sectors)
    mbr[446:462] = entry
    mbr[510:512] = b"\x55\xAA"
    return bytes(mbr)


def build_image(files: dict[str, bytes], size_mib: int = DEFAULT_SIZE_MIB) -> bytes:
    total_sectors = size_mib * 1024 * 1024 // SECTOR
    partition_sectors = total_sectors - PARTITION_START
    if partition_sectors <= 0:
        raise ImageError("image size is too small")
    fat = build_fat16(files, partition_sectors)
    # sector 0 is the MBR, sectors 1..PARTITION_START-1 are the alignment gap;
    # the FAT16 volume named in the partition table starts at PARTITION_START.
    image = bytearray(build_mbr(partition_sectors))
    image += bytes(PARTITION_START * SECTOR - SECTOR)
    image += fat
    if len(image) != total_sectors * SECTOR:
        raise ImageError("internal error: image size does not match the partition table")
    return bytes(image)


def collect_boot_files(firmware_dir: Path, kernel: Path, extra: list[Path],
                       config: Path | None, cmdline: Path | None) -> dict[str, bytes]:
    files: dict[str, bytes] = {"kernel.img": kernel.read_bytes()}

    for name in REQUIRED_BOOT_FILES:
        if name == "kernel.img":
            continue
        candidate = firmware_dir / name
        if not candidate.is_file():
            raise ImageError(
                f"firmware file '{name}' not found in {firmware_dir}.  Run "
                "tools/fetch-firmware.sh (needs network access), point "
                "LUME_FIRMWARE_DIR at a Raspberry Pi OS boot partition, or copy "
                "the files by hand: bootcode.bin, start.elf, fixup.dat")
        files[name] = candidate.read_bytes()

    if config and config.is_file():
        files["config.txt"] = config.read_bytes()
    if cmdline and cmdline.is_file():
        files["cmdline.txt"] = cmdline.read_bytes()

    for path in extra:
        files[path.name] = path.read_bytes()

    return files


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--kernel", required=True, type=Path,
                        help="kernel image (build/kernel.img)")
    parser.add_argument("--firmware-dir", type=Path, default=Path(".firmware"))
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--size-mib", type=int, default=DEFAULT_SIZE_MIB)
    parser.add_argument("--config", type=Path, default=Path("config.txt"))
    parser.add_argument("--cmdline", type=Path, default=Path("cmdline.txt"))
    parser.add_argument("--extra", type=Path, nargs="*", default=[],
                        help="additional files to place in the boot partition")
    args = parser.parse_args(argv)

    try:
        files = collect_boot_files(args.firmware_dir, args.kernel, args.extra,
                                   args.config, args.cmdline)
        image = build_image(files, args.size_mib)
    except (ImageError, OSError) as exc:
        print(f"mkimage: error: {exc}", file=sys.stderr)
        return 1

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(image)
    print(f"mkimage: wrote {args.out} ({len(image)} bytes, "
          f"{len(files)} files: {', '.join(sorted(files))})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
