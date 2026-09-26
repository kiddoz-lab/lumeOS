# Building LumeOS

This document describes every supported way to build LumeOS, what each build
target produces, and what to do when something fails. It is written for the
state of the repository *today*: the kernel builds and links, host tests run,
and the SD card image is produced by `make image`. There is no userspace yet, so
there is no userspace build step (see [roadmap.md](roadmap.md)).

---

## 1. What you need

| Requirement | Why | Notes |
| --- | --- | --- |
| `arm-none-eabi-gcc` + `arm-none-eabi-ld` | compiles and links the kernel | Debian/Ubuntu: `gcc-arm-none-eabi binutils-arm-none-eabi` |
| GNU `make` | drives the build | any POSIX make will do |
| Python 3.9 or newer | all build tools are Python | `python3` by default, override with `PYTHON=` |
| [capstone](https://www.capstone-engine.org/) (Python bindings) | disassembles the linked kernel for the ARMv6 ISA gate | Debian/Ubuntu: `python3-capstone`; otherwise `pip install capstone` |
| `readelf` (binutils) | build attributes and symbol table for the gates | `binutils-arm-none-eabi` or any binutils |
| `git` | embeds the commit id in the kernel image | the build works outside a git tree too, see §6 |
| Raspberry Pi boot firmware | only for `make image` | downloaded by `tools/fetch-firmware.sh`, see §5 |
| `qemu-system-arm` with the `raspi0` machine | only for `make test-qemu` | Debian/Ubuntu: `qemu-system-arm` (7.0 or newer) |

Nothing else is required. The repository deliberately has **no** submodules, no
vendored toolchain and no pip `requirements.txt`: the tools in `tools/` are
plain Python modules with at most one optional dependency (capstone).

### The no-root alternative (zig)

If you cannot install `gcc-arm-none-eabi` (no root, locked-down machine, CI
image without the package), the same build runs on [zig](https://ziglang.org),
which ships clang, lld and an ARM assembler:

```sh
make CC=tools/zig-cc.sh PYTHON=/path/to/python-with-capstone kernel
```

`tools/zig-cc.sh` is a thin wrapper around `zig cc`:

* it targets `arm-freestanding-eabi` with `-mcpu=arm1176jzf_s` (exactly the
  ARM1176JZF-S in the BCM2835);
* it translates the two GCC-style options clang spells differently
  (`-mcpu=arm1176jzf-s` to the underscore form, and it drops the linker-only
  `-Wl,--no-warn-rwx-segments` that lld does not understand);
* it disables Zig's default UBSan instrumentation, which would otherwise
  reference runtime symbols that a `-nostdlib` kernel cannot resolve.

You can point it at a specific zig binary with `LUME_ZIG=/path/to/zig`.

**This is the toolchain used for local verification during development**, so it
is a first-class path, not a hack. CI uses the GNU toolchain, so both
configurations are exercised regularly. Whichever you use, the *same* static
gates run on the result (see §4), so the two toolchains cannot silently diverge
on instruction set.

---

## 2. Build targets

Run `make help` for the summary; this is the detail.

| Target | Produces | What it does |
| --- | --- | --- |
| `make` (or `make all`) | `build/kernel.img`, `build/lumeos-sd.img` | `kernel` followed by `image` |
| `make kernel` | `build/lumeos.elf`, `build/kernel.img` | builds the user program first (the image embeds it), compiles every kernel source, links with `kernel/ld/lumeos.ld`, runs the two static gates on the kernel, then converts the ELF to the flat image |
| `make userspace` | `build/userspace/init.elf` | builds the first user program: a static, freestanding ARMv6 ELF with its own `crt0`, and runs the same ISA and EABI gates on it (`--allow-none` for the EABI one, see §4) |
| `make image` | `build/lumeos-sd.img` | fills a 64 MiB MBR + FAT16 volume with the firmware, `config.txt` and `kernel.img`, then verifies it |
| `make firmware` | `.firmware/` | downloads the pinned Raspberry Pi boot firmware |
| `make test` | – | alias for `test-host` |
| `make test-host` | – | runs the host unit tests (`tests/host`) |
| `make test-qemu` | – | boots `build/kernel.img` under QEMU, checks the boot and userspace markers and fails on any QEMU guest error |
| `make clean` | – | removes `build/` |
| `make distclean` | – | `clean` plus `.firmware/` |

Everything is built from sources in the repository; there is no generated
source step, no binary blob in the tree, and no configuration script.

### Useful variables

| Variable | Default | Meaning |
| --- | --- | --- |
| `CROSS_COMPILE` | `arm-none-eabi-` | toolchain prefix |
| `CC` | `$(CROSS_COMPILE)gcc` | compiler; set to `tools/zig-cc.sh` for the zig path |
| `PYTHON` | `python3` | interpreter for the tools in `tools/` |
| `CHECK_ISA_FLAGS` | `--require-attributes` | arguments for the ISA gate; CI uses `--require-attributes --require-capstone` |
| `CHECK_ABI_FLAGS` | `--require-capstone` | arguments for the EABI gate; the same rule, so a machine without capstone fails the build instead of silently skipping the disassembly half |
| `LUME_FIRMWARE_DIR` | `.firmware` | where the Raspberry Pi firmware lives |
| `BUILD` | `build` | output directory |

Example invocations:

```sh
# build with GNU tools
make kernel

# build with zig, using a virtualenv that has capstone
make CC=tools/zig-cc.sh PYTHON=.venv/bin/python kernel

# be strict about the ISA gate locally, exactly like CI
make kernel CHECK_ISA_FLAGS="--require-attributes --require-capstone" CHECK_ABI_FLAGS=--require-capstone
```

---

## 3. Compiler flags, and why each one is there

`make kernel` compiles with this set (`Makefile`, `ARCHFLAGS`/`COMMONFLAGS`):

```
-march=armv6kz -mcpu=arm1176jzf-s -marm -mlittle-endian -mfloat-abi=soft
-mno-unaligned-access
-ffreestanding -fno-builtin -fno-common -fno-stack-protector
-fno-omit-frame-pointer -Wall -Wextra -Wundef
-Werror=implicit-function-declaration -Wno-unused-parameter -std=gnu11 -O2 -g3
-I kernel/include -I kernel/arch/arm/include -I build/include
-nostdinc -nostdlib -fno-pic -fno-pie
```

| Flag | Reason |
| --- | --- |
| `-march=armv6kz -mcpu=arm1176jzf-s` | the real target. Pinned so a toolchain default change can never emit ARMv7+ instructions. |
| `-marm` | the kernel runs in ARM (not Thumb) state; `vectors.S` and `boot.S` assume it. |
| `-mfloat-abi=soft` | ARM1176JZF-S has **no** VFP/NEON. Hardware floating point is not "not used", it does not exist on this SoC. The kernel contains no floating point at all. |
| `-mno-unaligned-access` | `boot.S` sets `SCTLR.A`, so an unaligned access would fault. The compiler must not invent one. |
| `-ffreestanding -nostdinc` | there is no libc and no hosted headers. All headers come from `kernel/include`. |
| `-fno-builtin` | otherwise the compiler may replace a hand-written `memcpy`/`strlen` call with its own, or worse, call a libc symbol that does not exist. |
| `-fno-common -fno-stack-protector` | no runtime support code is linked, so neither feature may be enabled. |
| `-fno-omit-frame-pointer` | a fault dumped from a trap frame is far easier to read with a proper frame chain. |
| `-nostdlib -fno-pic -fno-pie` | the kernel is a fixed-address image; no PLT, no GOT, no dynamic loader. |
| `-g3` | debug information for `arm-none-eabi-addr2line`. The QEMU diagnosis pipeline in CI depends on it. |
| `-Werror=implicit-function-declaration` | a missing prototype in a freestanding kernel is almost always a real bug. |

The linker step uses `-Wl,--build-id=none` (a build id would add bytes to the
flat image with no use on a bare board), `-Wl,--Map=build/lumeos.map`, and
`-Wl,--no-warn-rwx-segments` (the image is intentionally one RWX `PT_LOAD`
segment - see [architecture.md](architecture.md)).

### Two build knobs that matter

* **The build is reproducible in the way that matters**: no `__DATE__`/`__TIME__`
  anywhere. The only build-dependent string is the git commit, embedded through
  `-DLUME_BUILD_COMMIT` and printed by `uname`/`version`.
* **The firmware is pinned**, not "latest": `tools/fetch-firmware.sh` downloads
  from a fixed commit of `raspberrypi/firmware` so that an image built today and
  an image built next month contain the same boot code.

---

## 4. The static gates every build runs

`make kernel` finishes by running two checkers on the linked ELF, and **fails
the build** if either reports a problem. This is not optional; both exist
because a mistake in either area compiles cleanly and then fails on hardware.

### `tools/check_isa.py` - is every instruction legal on ARMv6?

```sh
python3 tools/check_isa.py --require-attributes --require-capstone build/lumeos.elf
```

* Reads the ARM build attributes and requires `Tag_CPU_arch` to be one of the
  ARMv6 flavours the ARM1176JZF-S implements (`v6`, `v6K`, `v6KZ`).
* Disassembles **every executable section** with capstone and rejects
  instructions that do not exist on ARMv6 (`sdiv`, `udiv`, `cbz`, `movw`,
  `dmb`, `dsb`, `isb`, `wfe`, `sev`, VFP/NEON, the `*16` DSP forms, ...).
* Steps over embedded data: capstone's ARM decoder stops at the first word it
  cannot decode, so the scan uses `skipdata` and ignores `.byte`
  pseudo-instructions. Without that, a literal pool early in `.text` silently
  truncates the check (it once inspected 235 of 10231 instructions).

`--require-capstone` makes a missing capstone a failure instead of a warning.
CI always passes it; pass it locally too when you want the same strictness.

### `tools/check_abi.py` - do the EABI helpers match their callers?

```sh
python3 tools/check_abi.py --require-capstone build/lumeos.elf
```

The compiler calls `__aeabi_uidiv`, `__aeabi_uldivmod` and friends with the
register layout fixed by the ARM run-time ABI, which is *not* the layout this
toolchain uses for an equivalent C function (it returns composites in memory
through a hidden pointer). This checker disassembles the helpers and their call
sites and fails if a helper stores its result through `r0`, or if a caller
dereferences `r0` right after a multi-word helper call.

Both classes of bug are invisible to the compiler and the linker. The second
one actually happened here: it panicked the kernel with "64-bit division by
zero" before the serial console existed. See
[architecture.md](architecture.md#the-two-arm-calling-conventions-and-why-the-eabi-helpers-are-assembly).

### The same gates on the user program

`make userspace` runs both checkers on `build/userspace/init.elf`, because a
user program can emit an ARMv7-only instruction just as easily as the kernel
can - and there it would fault in *user* mode, where the failure is attributed
to the program rather than to the build. The EABI checker is invoked with
`--allow-none`: a program linked with `-nostdlib` that never calls a division
helper legitimately defines none of the six, and the linker is what guarantees
closure there. If such a program ever *does* define one, its layout is checked
like the kernel's.

### `tools/md2html.py` - reading the documentation

```sh
python3 tools/md2html.py --all build/docs-view   # README.md + every docs/*.md
python3 -m http.server 8000 --directory build/docs-view
```

The documents in `docs/` are the project's record, and reading them should not
depend on having a Markdown renderer or on a viewer whose theme happens to put
dark text on a light background. This script turns them into self-contained HTML
with the colours spelled out, and it depends on nothing but the Python standard
library - no `pip install`, no network - because the machine that needs it is
often the one where installing things is the problem. It is a documentation tool
and never runs in CI or in the build.

---

## 5. Building the SD card image

The image needs three files that are **not** in this repository - the Raspberry
Pi boot firmware `bootcode.bin`, `start.elf` and `fixup.dat`:

```sh
# option 1: download the pinned revision (needs network access to github.com)
make firmware            # -> .firmware/{bootcode.bin,start.elf,fixup.dat}

# option 2: copy them from any Raspberry Pi OS card
mkdir -p .firmware
cp /media/$USER/boot/{bootcode.bin,start.elf,fixup.dat} .firmware/

make image               # -> build/lumeos-sd.img
```

`make image` does more than concatenate files:

1. builds the kernel first (`image: kernel`);
2. writes an MBR with one bootable FAT16 partition starting at sector 2048;
3. writes a FAT16 filesystem with the nine-character volume label `LUMEOS`,
   containing `BOOTCODE.BIN`, `START.ELF`, `FIXUP.DAT`, `CONFIG.TXT` and
   `KERNEL.IMG`;
4. runs `tools/verify_image.py` against the result, which re-reads the image the
   way the firmware would: the `0x55AA` MBR signature, exactly one FAT16
   partition covering the whole image, the BPB (sector/cluster sizes, FAT
   count), the root directory entries, each file read back through its FAT
   cluster chain, and finally that `KERNEL.IMG` is byte-for-byte the kernel that
   was just built (and, when the source firmware directory is present, that
   `BOOTCODE.BIN`/`START.ELF`/`FIXUP.DAT` match it). `make image` fails if any
   of that is wrong.

`.firmware/` and `build/` are both git-ignored: neither the downloaded firmware
nor build output belongs in the repository.

### Flashing

```sh
lsblk                                     # find the card, e.g. /dev/sdX or /dev/mmcblk0
sudo dd if=build/lumeos-sd.img of=/dev/sdX bs=4M conv=fsync status=progress
```

Double-check the device name before running `dd`: it overwrites the target
completely. See [hardware.md](hardware.md) for the card, the serial console
wiring and what to expect on first boot.

---

## 6. Troubleshooting

### `arm-none-eabi-gcc: command not found`

Install the toolchain, or build with zig:

```sh
make CC=tools/zig-cc.sh kernel
```

### `ModuleNotFoundError: No module named 'capstone'`

The ISA gate cannot disassemble anything. Either install capstone
(`sudo apt-get install python3-capstone`, or `pip install capstone`) or point
the build at an interpreter that has it:

```sh
make PYTHON=/path/to/venv/bin/python kernel
```

Without `--require-capstone` the gate prints a warning and continues with the
build-attribute check only. CI passes `--require-capstone`, so a build that only
works because capstone is missing *will* fail in CI.

### `check_isa: FAIL: ... 'uxtab' does not exist on ARMv6`

This is what the gate looks like when it works, and it used to be a false
positive in this repository - the extend-and-add instructions are ARMv6, as the
compiler itself confirms by emitting `uxtab` for `-mcpu=arm1176jzf-s`. They are
no longer on the deny list. A real failure here means the compiler emitted
something for a newer architecture; check that `-march=armv6kz
-mcpu=arm1176jzf-s` are still in `ARCHFLAGS` (a `CC=` wrapper that drops flags
is the usual cause).

### `check_isa: FAIL: Tag_CPU_name is '6KZ'`

GNU `readelf` synthesises the CPU *name* from `Tag_CPU_arch` for a linked
binary, so it reports `6KZ` where `llvm-readelf` reports `arm1176jzf-s`. The
gate accepts that: the authoritative checks are `Tag_CPU_arch` and the
instruction scan. Only a name that proves a *newer core family* (`cortex-a*`,
`armv7`, `aarch64`, ...) is rejected.

### `check_abi: FAIL: ... stores its result through r0`

An EABI helper in the kernel is written as a C function returning a struct, so
it uses the compiler's hidden-pointer convention while its callers use the EABI
register convention. The fix is to write that helper in assembly (see
`kernel/arch/arm/aeabi_div.S`) and keep only scalar arguments and pointer
results in C. Do **not** silence the check: the resulting kernel boots into a
panic before it can print anything.

### The link fails with "LumeOS kernel image is too large"

`kernel/ld/lumeos.ld` asserts that the image ends before physical `0xF0000`,
because the exception vector page lives there. You have outgrown the first
`0xF0000 - 0x8000` bytes of the address space. Move the vector page (and the
`VECTOR_PAGE_PA` constant in `kernel/arch/arm/boot.S`, `kernel/kernel/main.c`
and the linker script) somewhere free, or trim a feature; do not delete the
assertion.

### `make image` says the firmware directory is incomplete

`.firmware/` is missing `bootcode.bin`, `start.elf` or `fixup.dat`. Run
`make firmware`, or copy them off any Raspberry Pi OS card (§5).

### Warnings about `LUME_BUILD_COMMIT` / `git rev-parse` output

Outside a git working tree (for example in a source tarball) the commit id
becomes `unknown`. That is intentional: the build must not require git. Inside
CI, `actions/checkout` is configured with `fetch-depth: 0` so the id is real.

### The kernel is built but nothing appears on the serial port

That is a runtime question, not a build question - see
[hardware.md](hardware.md#no-output-at-all) and
[testing.md](testing.md#3-emulated-boot-test). Two things to check first anyway:
`config.txt` must contain `enable_uart=1` (and on a Pi Zero W
`dtoverlay=disable-bt`, because the PL011 is wired to the Bluetooth modem by
default), and the serial adapter must be connected to GPIO14/GPIO15 with GND,
at 115200 8N1, with the adapter's TX going to the Pi's RX.

---

## 7. What CI does

`.github/workflows/build.yml` runs on every push and pull request:

1. installs `gcc-arm-none-eabi`, `binutils-arm-none-eabi`, `qemu-system-arm`
   and `python3-capstone`;
2. `make kernel` with `CHECK_ISA_FLAGS=--require-attributes --require-capstone` and
   `CHECK_ABI_FLAGS=--require-capstone`;
   on failure, the tail of the log is republished as an annotation (the log
   download host is not reachable from every environment, annotations are);
3. re-runs `tools/elf2bin.py` to a second file, `cmp`s it against
   `build/kernel.img`, prints the physical load range, and runs `file` and
   `arm-none-eabi-size` on the ELF;
4. builds the user program on its own (`make userspace`), prints its ELF headers
   and program headers, and publishes what the kernel will load as an
   annotation - entry point, segments and size;
5. `make test-host`;
6. `make firmware` and `make image`, then `tools/verify_image.py`, which also
   publishes the image size, file list and SHA-256 as an annotation;
7. uploads `lumeos-sd-image` and `lumeos-kernel` artifacts (the kernel artifact
   includes `build/userspace/init.elf`);
8. runs the QEMU boot test as a real gate - losing a boot marker, a self test,
   the shell prompt, the user program's own output or a clean QEMU guest-error
   log fails the build. On failure the job also runs
   `tests/qemu/diagnose_boot.py` and publishes the diagnosis as an annotation
   and a log artifact, so a boot regression arrives with its own trace. See
   [testing.md](testing.md) for what this proves and what only hardware can - and
   for the guest-error gate, which is there because a write to a read-only
   interrupt register once booted the kernel perfectly and was still wrong.

A build of the SD image is therefore a CI artifact you can download and flash
without building anything locally. It is **not** evidence that the kernel boots:
nothing in this project has been run on real hardware, and the emulated boot
test does not pass yet.
