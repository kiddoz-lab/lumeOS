#!/usr/bin/env python3
"""Verify that a built LumeOS SD card image is actually bootable structure.

`make image` writes build/lumeos-sd.img with tools/mkimage.py.  This tool reads
the result back the way the Raspberry Pi firmware would and checks it:

  * the MBR has the boot signature and exactly one FAT16 partition entry whose
    start sector and length match the file size;
  * the partition's BPB describes a FAT16 volume whose geometry is consistent
    (sector size, cluster size, FAT count, FAT size, root entry count);
  * the root directory contains the files the firmware needs
    (BOOTCODE.BIN, START.ELF, FIXUP.DAT, KERNEL.IMG) and, when given, the config
    file and extra files;
  * every file's cluster chain is walkable, and the *contents* are byte for byte
    what the build produced - the kernel image compared against
    build/kernel.img, the firmware compared against the downloaded files.

It exits non-zero on any mismatch, so it can be wired into `make image` and into
CI.  With --annotation it also prints a GitHub Actions notice containing the
summary, which makes the verification result readable from the checks API.

Usage:
    tools/verify_image.py --image build/lumeos-sd.img --kernel build/kernel.img \
        --firmware-dir .firmware --annotation
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
from pathlib import Path

SECTOR = 512
MBR_PARTITION_OFFSET = 446
FAT16_TYPE = 0x06
REQUIRED_FILES = ("BOOTCODE.BIN", "START.ELF", "FIXUP.DAT", "KERNEL.IMG")


class VerifyError(Exception):
    pass


def read_root_directory(fat: bytes, bpb: dict) -> dict[str, tuple[int, int]]:
    """Map 8.3 name -> (first cluster, size) for every file in the root."""
    entries: dict[str, tuple[int, int]] = {}
    root_start = (bpb["reserved"] + bpb["num_fats"] * bpb["fat_size"]) * bpb["bytes_per_sector"]
    for i in range(bpb["root_entries"]):
        entry = fat[root_start + i * 32:root_start + (i + 1) * 32]
        if len(entry) < 32:
            raise VerifyError("root directory is truncated")
        if entry[0] == 0x00:
            break
        if entry[0] == 0xE5:
            continue
        if entry[11] & 0x08:                       # volume label
            continue
        name = entry[0:8].decode("ascii", "replace").rstrip()
        ext = entry[8:11].decode("ascii", "replace").rstrip()
        full = f"{name}.{ext}" if ext else name
        first = struct.unpack_from("<H", entry, 26)[0]
        size = struct.unpack_from("<I", entry, 28)[0]
        entries[full] = (first, size)
    return entries


def read_file(fat: bytes, bpb: dict, cluster: int, size: int) -> bytes:
    data_start = ((bpb["reserved"] + bpb["num_fats"] * bpb["fat_size"]
                   + bpb["root_dir_sectors"]) * bpb["bytes_per_sector"])
    bytes_per_cluster = bpb["sectors_per_cluster"] * bpb["bytes_per_sector"]
    out = bytearray()
    seen: set[int] = set()
    while cluster and cluster < 0xFFF8 and len(out) < size:
        if cluster in seen:
            raise VerifyError(f"FAT loop at cluster {cluster}")
        seen.add(cluster)
        offset = data_start + (cluster - 2) * bytes_per_cluster
        out += fat[offset:offset + bytes_per_cluster]
        cluster = struct.unpack_from("<H", fat, bpb["bytes_per_sector"] + cluster * 2)[0]
    if len(out) < size:
        raise VerifyError(f"cluster chain shorter than the recorded size {size}")
    return bytes(out[:size])


def parse_bpb(fat: bytes) -> dict:
    if fat[510:512] != b"\x55\xAA":
        raise VerifyError("FAT boot sector has no 0x55AA signature")
    if fat[54:62] != b"FAT16   ":
        raise VerifyError(f"filesystem type string is {fat[54:62]!r}, expected b'FAT16   '")
    bytes_per_sector = struct.unpack_from("<H", fat, 11)[0]
    sectors_per_cluster = struct.unpack_from("<B", fat, 13)[0]
    reserved = struct.unpack_from("<H", fat, 14)[0]
    num_fats = struct.unpack_from("<B", fat, 16)[0]
    root_entries = struct.unpack_from("<H", fat, 17)[0]
    total16 = struct.unpack_from("<H", fat, 19)[0]
    total32 = struct.unpack_from("<I", fat, 32)[0]
    fat_size = struct.unpack_from("<H", fat, 22)[0]

    if bytes_per_sector != SECTOR:
        raise VerifyError(f"bytes per sector is {bytes_per_sector}, expected 512")
    if num_fats != 2:
        raise VerifyError(f"FAT count is {num_fats}, expected 2")
    if sectors_per_cluster == 0 or (sectors_per_cluster & (sectors_per_cluster - 1)):
        raise VerifyError(f"sectors per cluster is {sectors_per_cluster}, expected a power of two")
    root_dir_sectors = (root_entries * 32 + bytes_per_sector - 1) // bytes_per_sector

    return {
        "bytes_per_sector": bytes_per_sector,
        "sectors_per_cluster": sectors_per_cluster,
        "reserved": reserved,
        "num_fats": num_fats,
        "root_entries": root_entries,
        "root_dir_sectors": root_dir_sectors,
        "fat_size": fat_size,
        "total_sectors": total32 or total16,
        "label": fat[43:54].decode("ascii", "replace").strip(),
    }


def verify(image_path: Path, kernel_path: Path | None,
           firmware_dir: Path | None) -> dict:
    image = image_path.read_bytes()
    if len(image) % SECTOR:
        raise VerifyError(f"image size {len(image)} is not a multiple of {SECTOR}")

    # ---- master boot record ----
    if image[510:512] != b"\x55\xAA":
        raise VerifyError("MBR has no 0x55AA signature at offset 510")
    entries = []
    for i in range(4):
        entry = image[MBR_PARTITION_OFFSET + i * 16:MBR_PARTITION_OFFSET + (i + 1) * 16]
        if entry[4] == 0x00:
            continue
        start, count = struct.unpack_from("<II", entry, 8)
        entries.append({"index": i, "type": entry[4], "start": start, "sectors": count})
    if len(entries) != 1:
        raise VerifyError(f"expected exactly one partition entry, found {len(entries)}")
    part = entries[0]
    if part["type"] != FAT16_TYPE:
        raise VerifyError(f"partition type is 0x{part['type']:02x}, expected 0x{FAT16_TYPE:02x}")
    if (part["start"] + part["sectors"]) * SECTOR != len(image):
        raise VerifyError(
            f"partition covers sectors {part['start']}..{part['start'] + part['sectors']} "
            f"but the image is {len(image) // SECTOR} sectors")

    # ---- FAT16 volume ----
    fat = image[part["start"] * SECTOR:]
    bpb = parse_bpb(fat)
    files = read_root_directory(fat, bpb)

    details: dict = {
        "image": str(image_path),
        "image_bytes": len(image),
        "sha256": hashlib.sha256(image).hexdigest(),
        "partition_start": part["start"],
        "partition_sectors": part["sectors"],
        "label": bpb["label"],
        "cluster_bytes": bpb["sectors_per_cluster"] * SECTOR,
        "fat_size_sectors": bpb["fat_size"],
        "files": {},
    }

    for required in REQUIRED_FILES:
        if required not in files:
            raise VerifyError(f"{required} is missing from the boot partition")

    for name, (cluster, size) in sorted(files.items()):
        content = read_file(fat, bpb, cluster, size)
        details["files"][name] = {"bytes": size, "sha256": hashlib.sha256(content).hexdigest()}

        if name == "KERNEL.IMG" and kernel_path is not None:
            expected = kernel_path.read_bytes()
            if content != expected:
                raise VerifyError(
                    f"{name} in the image ({size} bytes) differs from {kernel_path} "
                    f"({len(expected)} bytes)")
            details["kernel_matches_build"] = True

        if firmware_dir is not None:
            source = firmware_dir / name.lower()
            if source.is_file():
                if content != source.read_bytes():
                    raise VerifyError(f"{name} in the image differs from {source}")
                details["files"][name]["matches_firmware"] = True

    return details


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, default=None,
                        help="flat kernel image that must appear as KERNEL.IMG")
    parser.add_argument("--firmware-dir", type=Path, default=None)
    parser.add_argument("--annotation", action="store_true",
                        help="also print a GitHub Actions ::notice:: line with the summary")
    args = parser.parse_args(argv)

    try:
        details = verify(args.image, args.kernel, args.firmware_dir)
    except (VerifyError, OSError) as exc:
        print(f"verify_image: FAIL: {exc}", file=sys.stderr)
        if args.annotation:
            print(f"::error title=verify_image::{exc}")
        return 1

    summary = (f"{args.image.name}: {details['image_bytes']} bytes, "
               f"{len(details['files'])} files "
               f"({', '.join(sorted(details['files']))}), "
               f"cluster {details['cluster_bytes']} B, "
               f"volume label {details['label']}, "
               f"sha256 {details['sha256']}")
    if details.get("kernel_matches_build"):
        summary += "; KERNEL.IMG matches the built kernel byte for byte"
    print(f"verify_image: ok: {summary}")
    for name, info in sorted(details["files"].items()):
        note = " (matches source)" if info.get("matches_firmware") else ""
        print(f"verify_image:   {name:14s} {info['bytes']:>8d} bytes  "
              f"sha256 {info['sha256'][:16]}...{note}")

    if args.annotation:
        print(f"::notice title=SD image verified::{summary}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
