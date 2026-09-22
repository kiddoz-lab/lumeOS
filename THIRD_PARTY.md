# Third-party components and licences

LumeOS itself is MIT licensed - see [LICENSE](LICENSE). The kernel contains no
third-party source code: every C and assembly file in `kernel/` was written for
this project from public specifications (listed in
[docs/research-notes.md](docs/research-notes.md)).

This file records everything *external* the project depends on, downloads, or
uses as a reference, and the licence that applies to each. It is kept accurate
rather than aspirational: if the situation changes, this file changes with it.

---

## 1. Downloaded at build time (not redistributed by this repository)

### Raspberry Pi boot firmware

| | |
| --- | --- |
| Files | `boot/bootcode.bin`, `boot/start.elf`, `boot/fixup.dat` |
| Source | [`raspberrypi/firmware`](https://github.com/raspberrypi/firmware) (the `master` branch of the official Raspberry Pi firmware repository) |
| Pinned revision | `bead686816848038563a542dc854346ab13253a2` (`tools/fetch-firmware.sh`) |
| Licence | Copyright (c) 2012-2026 Raspberry Pi (Trading) Ltd. Distributed under the terms in the firmware repository (`boot/LICENCE.broadcom` for the Broadcom/VideoCore components). It is **not** freely redistributable as project source, which is why this repository does not contain it |
| How LumeOS uses it | The SD card image produced by `make image` contains the three files, because the BCM2835 GPU boots them from ROM before the ARM starts. They are required for the board to boot at all and are not linked with, derived from, or modified by LumeOS. `tools/fetch-firmware.sh` downloads them at the pinned revision, or you can copy them from any Raspberry Pi OS card |

`build/lumeos-sd.img` therefore contains Raspberry Pi firmware alongside the
MIT-licensed LumeOS kernel. If you redistribute that image, the firmware's
terms apply to those three files.

### ARM toolchain (optional, build-time only)

`arm-none-eabi-gcc` / `binutils-arm-none-eabi` (GNU Arm Embedded Toolchain,
GPL-3.0-or-later with the GCC Runtime Library Exception) or
[zig](https://ziglang.org) (MIT). Either can build LumeOS; neither is vendored
here, and no toolchain code is linked into the kernel. `tools/zig-cc.sh` is a
wrapper that only translates command-line flags.

The compiler's runtime helpers ("libgcc") are **not** linked: the EABI helpers
the kernel needs (`__aeabi_uidiv`, `__aeabi_uldivmod`, the `mem*` family) are
implemented in `kernel/kernel/aeabi.c`, `kernel/kernel/divmod.c` and
`kernel/arch/arm/aeabi_div.S` from the published ARM run-time ABI, and are
covered by tests. Details in
[docs/architecture.md](docs/architecture.md#the-two-arm-calling-conventions-and-why-the-eabi-helpers-are-assembly).

---

## 2. Build and test dependencies (not linked into the kernel)

| Component | Licence | Used by | Notes |
| --- | --- | --- | --- |
| [capstone](https://www.capstone-engine.org/) (Python bindings) | BSD-3-Clause | `tools/check_isa.py`, `tools/check_abi.py` | disassembles the linked kernel to reject ARMv7+ instructions and to verify the EABI helpers. Optional: without it the ISA gate degrades to a warning unless `--require-capstone` is passed (CI passes it) |
| Python 3 (standard library only) | PSF-2.0 | every tool in `tools/`, every test | the image builder, the ELF converter, the verifiers and the CI annotation helper are plain standard-library Python; there is no `requirements.txt` and no vendored package |
| GNU `make` | GPL-3.0-or-later | the build | - |
| `readelf` (binutils) | GPL-3.0-or-later | build attributes and symbol tables for both gates | - |
| [QEMU](https://www.qemu.org) (`qemu-system-arm`) | GPL-2.0 | `tests/qemu/run_qemu_test.py`, `tests/qemu/diagnose_boot.py` | only an emulator used for testing; not present on the target |
| GitHub Actions (`actions/checkout`, `actions/upload-artifact`) | MIT | CI | - |

No third-party library is compiled into the kernel, and the kernel has no
runtime dependencies of any kind.

---

## 3. Documentation and specifications consulted

Reusing an *interface definition* is not copying code, but the sources are worth
naming because the project depends on them being accurate:

| Source | Licence / terms | How it is used |
| --- | --- | --- |
| *BCM2835 ARM Peripherals* (Broadcom) | publicly published datasheet; Broadcom's terms for the document | register addresses, bit fields and peripheral behaviour. Reproduced only as constants, with chapter references in comments |
| *ARM1176JZF-S Technical Reference Manual*, *ARM Architecture Reference Manual (ARMv6)*, *Run-time ABI for the ARM Architecture*, *AAPCS* (Arm Ltd) | publicly published architecture documentation, distributed by Arm under its documentation terms | exception and MMU behaviour, instruction availability, the fixed register conventions of the `__aeabi_*` helpers |
| *Raspberry Pi Documentation* (`config.txt`, boot process, UART, mailbox property interface) | © Raspberry Pi Ltd, published for public use | the boot chain, `config.txt` options, and the mailbox protocol the firmware implements |
| Linux kernel UAPI headers (`arch/arm/include/uapi/asm/*.h`, `include/uapi/linux/*.h`, `include/uapi/asm-generic/*.h`) | the kernel's userspace-ABI headers are distributed under the **Linux syscall note** exception: they may be used by non-GPL programs to describe the interface to the kernel (the headers themselves carry `SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note`) | syscall numbers, `errno` values, `struct stat64`/`utsname`/`sigaction` layouts and flag definitions for the Linux-compatibility layer. **No Linux kernel implementation source is copied into LumeOS**, and LumeOS is not derived from the Linux kernel - see [docs/userspace.md](docs/userspace.md) and [docs/research-notes.md](docs/research-notes.md) |
| QEMU sources (`hw/arm/raspi.c`, `hw/arm/boot.c`, `hw/arm/bcm2835_peripherals.c`, `hw/timer/bcm2835_systmr.c`, `hw/misc/bcm2835_property.c`) | GPL-2.0 | consulted to understand what the emulator models and where it disagrees with the datasheet. No QEMU code is copied or linked |
| `raspberrypi/firmware` repository metadata | see §1 | locating the boot files and pinning a revision |

The project's rule is that an interface may be implemented from its
specification, but kernel *implementation* code from another operating system is
not copied, paraphrased or transliterated. Where a comment in the source names a
documented register or structure, the corresponding source is listed above.

---

## 4. What is deliberately absent

* No Linux kernel source, no `linux/` tree, no kernel module headers.
* No bundled bootloader, no vendored toolchain, no prebuilt binaries.
* No GPL code linked into the kernel, so the kernel's licence is unaffected by
  any of the above.
* No redistribution of the Raspberry Pi firmware: it is downloaded at build time
  (or copied by the user), and it lives only in `.firmware/` (git-ignored) and in
  the built SD image.

If you believe something here is misattributed or missing, open an issue - the
file exists precisely so that this can be checked.
