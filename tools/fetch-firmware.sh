#!/bin/sh
# Fetch the Raspberry Pi boot firmware for the LumeOS SD card image.
#
# The firmware (bootcode.bin, start.elf, fixup.dat) is not part of LumeOS and
# is not redistributed in this repository: it is downloaded from the official
# Raspberry Pi firmware repository and stays under its own licence, which is
# documented in THIRD_PARTY.md.
#
# Two ways to get the files:
#   1. this script (needs network access to github.com);
#   2. copy them from any Raspberry Pi OS SD card - the same three files live
#      in the boot partition - into the directory given below.
#
# Usage: tools/fetch-firmware.sh [destination-directory]
#        LUME_FIRMWARE_REF=<commit> tools/fetch-firmware.sh .firmware
set -eu

DEST="${1:-.firmware}"
# Pinned commit of raspberrypi/firmware (master moves; a boot image build
# should be reproducible).  This is the commit that was verified to contain
# boot/bootcode.bin, boot/start.elf and boot/fixup.dat:
#   bead686816848038563a542dc854346ab13253a2  2026-09-15  "kernel: Bump to 6.18.52"
# Update deliberately (check that all three files exist at the new commit),
# never automatically.  Override for a one-off build with LUME_FIRMWARE_REF.
REF="${LUME_FIRMWARE_REF:-bead686816848038563a542dc854346ab13253a2}"
BASE="https://github.com/raspberrypi/firmware/raw/${REF}/boot"
FILES="bootcode.bin start.elf fixup.dat"

mkdir -p "$DEST"

if command -v curl >/dev/null 2>&1; then
    FETCH="curl -fL --retry 3 --retry-delay 2 -o"
elif command -v wget >/dev/null 2>&1; then
    FETCH="wget -q -O"
else
    echo "fetch-firmware: need curl or wget" >&2
    exit 1
fi

for file in $FILES; do
    if [ -s "$DEST/$file" ]; then
        echo "fetch-firmware: $DEST/$file already present, skipping"
        continue
    fi
    echo "fetch-firmware: downloading $file"
    # shellcheck disable=SC2086
    $FETCH "$DEST/$file" "$BASE/$file" || {
        echo "fetch-firmware: failed to download $file." >&2
        echo "  Copy bootcode.bin, start.elf and fixup.dat from a Raspberry Pi" >&2
        echo "  OS boot partition into $DEST instead." >&2
        exit 1
    }
done

printf '%s\n' "$REF" > "$DEST/.lume-firmware-ref"
echo "fetch-firmware: firmware ready in $DEST (raspberrypi/firmware@$REF)"
ls -l "$DEST"
