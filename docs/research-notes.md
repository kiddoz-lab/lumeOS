# Research notes

Everything LumeOS implements was built from public documentation. This file
records what was consulted, what was taken from each source, and what remains
uncertain, so that a reader (or a future maintainer) can retrace the reasoning
instead of trusting the code.

It doubles as the list of things that were *checked* rather than assumed. Where
a fact was verified in an emulator rather than on hardware, that is said
explicitly.

---

## 1. Primary hardware documentation

| Source | What LumeOS takes from it |
| --- | --- |
| **BCM2835 ARM Peripherals** (Broadcom, C6357-M-1398) | the whole register map: interrupt controller (chapter 7), GPIO (chapter 6), system timer (chapter 12), PL011 UART (chapter 13), power management/watchdog/reset (chapter 13), the peripheral base `0x7E000000` |
| **ARM1176JZF-S Technical Reference Manual** (ARM DDI 0301) | CP15 system control (SCTLR, TTBR, DACR, fault status and address registers), exception entry/return, `SRS`/`RFE` availability on ARMv6, the L1 section/page descriptor formats in `pte.h` |
| **ARM Architecture Reference Manual, ARMv6 edition** | instruction availability (why `sdiv`, `cbz`, `dmb`, the `*16` DSP forms and VFP are rejected by `tools/check_isa.py`), ARMv6 fault status encoding |
| **ARMs "Run-time ABI for the ARM Architecture" (rtabi) addendum** | the fixed register conventions of `__aeabi_uidiv`, `__aeabi_uidivmod`, `__aeabi_uldivmod`, `__aeabi_ldivmod`, and the AAPCS rules the kernel's assembler stubs follow. This is also the source of the mistake documented in [architecture.md](architecture.md#the-two-arm-calling-conventions-and-why-the-eabi-helpers-are-assembly) |
| **AAPCS** (Procedure Call Standard for the ARM Architecture) | 8-byte stack alignment, parameter/return conventions, why the kernel's thunks push an even number of registers |

### Raspberry Pi specific

| Source | What LumeOS takes from it |
| --- | --- |
| Raspberry Pi documentation, `config.txt` (the legacy 32-bit options) | `kernel_address` default `0x8000`, `kernel_old`, `disable_commandline_tags`, `enable_uart`, `uart_2ndstage`, `disable_l2cache` |
| Raspberry Pi documentation, "UART" pages, and the `dtoverlay=disable-bt` / `miniuart-bt` overlays | the PL011-vs-mini-UART routing on BT-equipped boards. This is a hardware trap the emulator cannot reproduce: LumeOS drives the PL011, so on a Pi Zero W the boot configuration must hand the PL011 to GPIO14/15 |
| Raspberry Pi firmware wiki, "Mailbox property interface" | the mailbox protocol: 16-byte alignment, the size field counting itself, one response word per tag, `0x80000000` for success, and the tag numbers used in `mbox.h` (`GET_ARM_MEMORY` `0x00010005`, `GET_CLOCK_RATE` `0x00030002`, framebuffer `0x00048003`/`0x00048004`/`0x00048005`/`0x00048009`/`0x00040008`/`0x00040001`) |
| `raspberrypi/firmware` repository | the boot files themselves (`bootcode.bin`, `start.elf`, `fixup.dat`), pinned by commit in `tools/fetch-firmware.sh`. Not redistributed here - see [THIRD_PARTY.md](../THIRD_PARTY.md) |
| Community write-ups of the boot chain (forum threads, bare-metal tutorials) | corroboration of the load address and the "firmware loads and jumps" model; every such claim is treated as unverified until the board says so |

---

## 2. The ARM Linux ABI (for the userspace target)

Taken from the Linux kernel's UAPI headers, which are the normative description
of the userspace ABI:

| Header | Used for |
| --- | --- |
| `arch/arm/include/uapi/asm/unistd.h` | syscall numbers and the `__ARM_NR_*` private range (`__ARM_NR_BASE` = `0x0f0000`, `set_tls` = `+5`) |
| `arch/arm/include/uapi/asm/stat.h` | `struct stat64`: 64 bytes on ARM, with `st_mode` at 16 and `st_size` at 44 |
| `arch/arm/include/uapi/asm/signal.h` | `struct sigaction` for 32-bit ARM (`handler`, `sigset_t`, `flags`, `restorer`) and the signal numbers used by the fault path (`SIGSEGV` 11, `SIGBUS` 7, `SIGILL` 4, `SIGSYS` 31) |
| `arch/arm/include/uapi/asm/hwcap.h` | the `HWCAP_*` bits that must describe an ARMv6 CPU with no VFP honestly |
| `include/uapi/linux/utsname.h` | the 6x65-byte `struct utsname` that `uname(2)` fills in |
| `include/uapi/asm-generic/errno.h`, `fcntl.h`, `mman-common.h` | `errno` values (`EINTR`, `ENOSYS`, `EFAULT`, `EINVAL`...), `O_*` flags, `PROT_*`/`MAP_*` flags |

The kernels' *implementation* was not consulted for LumeOS's own design: the
kernel is not derived from Linux, no Linux source is copied, and none of it
links against or emulates Linux internally. The UAPI headers are used the way a
compiler's ABI documents are used - as the published interface definition.

### ABI facts worth having in one place

* `svc #0`, number in `r7`, arguments `r0-r5`, return in `r0`,
  `[-4095, -1]` = `-errno`.
* `mmap2` takes a **page** offset, not a byte offset.
* `__ARM_NR_BASE` = `0x0f0000`; the historic `0x900000 + n` space still exists
  in some binaries.
* glibc and musl both need `AT_HWCAP`, `AT_PAGESZ` and `AT_RANDOM` in `auxv`;
  a missing `AT_RANDOM` makes a hardened libc abort before `main`.
* TLS on ARM has several variants; the two worth supporting are
  `__ARM_NR_set_tls` and `CLONE_SETTLS`.
* `struct stat64` on ARM is *not* the same layout as on x86 - field order and
  padding differ, so a hand-written struct copied from another architecture is
  a silent corruption bug.

---

## 3. Emulator references (for the boot tests)

The QEMU sources are the reference for what the emulator does, which matters
because an emulator and a datasheet can disagree - and on this SoC they do:

| QEMU source | What it establishes |
| --- | --- |
| `hw/arm/raspi.c` | the `raspi0` machine is a BCM2835 with an ARM1176 (`board_rev` `0x920092`); with `-bios` the file is loaded at `0x8000` and entered there (`firmware_loaded`), which is the Pi-firmware hand-off model the boot test uses |
| `hw/arm/boot.c` | `-kernel` is **not** the same contract: for a raw binary QEMU uses its Linux boot path (loaded at `0x10000`, ATAGS at `0x100`, its own stub running first). That is why `tests/qemu/run_qemu_test.py` tries three strategies and why the `kernel` strategy must not be the only one |
| `hw/arm/bcm2835_peripherals.c` | which devices exist in the model, the PL011 is `serial_hd(0)` (so the kernel's UART is what `-serial stdio` carries), and - importantly - the **system timer's four compare channels are wired to the interrupt controller's GPU IRQ lines**, not to the shared IRQ numbers the datasheet describes |
| `hw/timer/bcm2835_systmr.c` | the timer model implements the free-running counter, the compare registers (`timer_mod(now + (value - now))`) and the interrupt; reading the control/status register reflects the match bits |
| `hw/misc/bcm2835_property.c` | which mailbox property tags the model answers (RAM size, clock rates, board revision, framebuffer) and which return "not implemented" |

Consequence, recorded as a design lesson: `timer.c` does **not** trust the IRQ
routing. It checks the timer's own control/status register inside the handler
whatever the interrupt source claims, and it offers `timer_poll_fallback()` for
the idle loop, counting how many ticks came from each path so the boot log shows
which one is doing the work on real hardware.

---

## 4. Third-party code and licences

LumeOS itself is MIT licensed ([LICENSE](../LICENSE)). It contains no
third-party source. Two things are external:

* **The Raspberry Pi boot firmware** (`bootcode.bin`, `start.elf`, `fixup.dat`):
  not redistributed, downloaded by `tools/fetch-firmware.sh` from the official
  repository under its own licence. The SD image built by `make image` contains
  those files, which is what makes it bootable.
* **capstone** (optional): used by `tools/check_isa.py` and
  `tools/check_abi.py` at build time only. No capstone code is linked into the
  kernel.

[THIRD_PARTY.md](../THIRD_PARTY.md) has the licence details.

---

## 5. Things that are still uncertain

Recorded honestly, because guessing here is how projects quietly become wrong:

1. **PL011 behaviour under continuous RX.** The emulator shows the RX interrupt
   being taken continuously with no data to read. Either the interrupt needs a
   different acknowledgement sequence than the driver performs, or the driver
   is missing a masking step; the manual's description of `MIS`/`ICR`/`IMSC`
   does not resolve it on its own.
2. **How the timer's IRQ line appears on real hardware** versus QEMU (GPU IRQ
   line vs shared IRQ numbers). Both paths are implemented; which one the board
   uses will be visible in the first hardware boot log, because the timer driver
   reports the split between interrupt-serviced and polled ticks.
3. **The interaction between the firmware's `enable_uart` clock setup and the
   PL011 divisor.** `uart_init()` asks the firmware for the UART clock over the
   mailbox and computes the divisor from the answer, with a documented fallback
   when the mailbox does not answer. Whether the firmware keeps that clock fixed
   on a Zero W with `enable_uart=1` is something only hardware will confirm.
4. **The real usable RAM.** The mailbox reports the ARM's share of the split;
   the kernel reserves the low 8 MiB, its own image and the vector page. The
   exact figure on a real board is unverified.
5. **Which UART the board actually routes** with `dtoverlay=disable-bt` plus the
   LumeOS `config.txt` - documented behaviour, but not yet observed here.
6. **Alignment and cache behaviour of the mailbox.** The mailbox is polled with
   explicit cache maintenance around the message buffer; the manual is
   ambiguous about whether the GPU ever writes the buffer with the ARM's
   D-cache marked clean, so the driver cleans and invalidates defensively.
