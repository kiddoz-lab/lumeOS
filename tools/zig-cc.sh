#!/bin/sh
# Compile and link LumeOS with the Zig toolchain instead of GNU arm-none-eabi.
#
# Zig ships clang, lld and an ARM assembler, which is everything this build
# needs.  It is used in two situations:
#
#   * a development machine without root rights, where the arm-none-eabi
#     packages cannot be installed (this is how the kernel was first built and
#     checked locally);
#   * CI as a fallback when the system cross toolchain is unavailable.
#
# Usage:  make CC=tools/zig-cc.sh PYTHON=/path/to/python-with-capstone
#    or:  sudo apt-get install gcc-arm-none-eabi   # then plain make
#
# The wrapper translates the two GCC-style options Zig's clang spells
# differently and adds the sanitizer switches the freestanding kernel build
# needs (Zig's C frontend enables UBSan by default, which references runtime
# symbols the kernel does not link).
set -eu

ZIG="${LUME_ZIG:-zig}"
if ! command -v "$ZIG" >/dev/null 2>&1; then
    for candidate in \
        "$HOME/.local/venv/bin/zig" \
        "$HOME/.local/venv/lib/python3.11/site-packages/ziglang/zig" \
        "$HOME/.local/bin/zig"
    do
        if [ -x "$candidate" ]; then
            ZIG="$candidate"
            break
        fi
    done
fi

if ! command -v "$ZIG" >/dev/null 2>&1; then
    echo "zig-cc: cannot find 'zig'; set LUME_ZIG=/path/to/zig" >&2
    exit 1
fi

TARGET="-target arm-freestanding-eabi"

# Extra flags: no sanitizers (freestanding, no libubsan), no stack protector.
EXTRA="-fno-sanitize=undefined -fno-sanitize-trap=undefined"

args=""
for arg in "$@"; do
    case "$arg" in
        # clang spells the CPU with an underscore.
        -mcpu=arm1176jzf-s) arg="-mcpu=arm1176jzf_s" ;;
        # GCC-only options that clang does not know; the architecture is
        # already pinned by -march=armv6kz -mcpu=arm1176jzf_s.
        -fno-pic|-fno-pie|-mthumb-interwork) continue ;;
        -Wl,--no-warn-rwx-segments) continue ;;
        # Zig's linker driver does not forward map-file options; the map is a
        # convenience for debugging, not something the build depends on.
        -Wl,-Map=*|-Wl,--Map=*)
            echo "zig-cc: note: skipping $arg (zig does not support map files)" >&2
            continue ;;
        *) ;;

    esac
    # shellcheck disable=SC2086
    args="$args $arg"
done

# shellcheck disable=SC2086
exec "$ZIG" cc $TARGET $EXTRA $args
