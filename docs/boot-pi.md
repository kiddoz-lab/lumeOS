# The Raspberry Pi boot chain, and what LumeOS assumes about it

A Pi Zero W does not boot the way a PC does, and the difference matters if you
are writing the kernel that runs at the end of the chain. This document
describes the chain from applying power to the first instruction of LumeOS,
lists every assumption LumeOS makes about it, and points at where each
assumption is implemented.

Everything here comes from public documentation (the Raspberry Pi documentation,
the BCM2835 ARM Peripherals manual, and the `raspberrypi/firmware` repository);
the reference list is in [research-notes.md](research-notes.md). None of it has
been verified on hardware by this project - see [testing.md](testing.md).

---

## 1. The chain

```
 power on
    |
    v
 VideoCore IV GPU boots from ROM         <- not the ARM. The ARM is held in reset
    |
    v
 reads bootcode.bin from the FAT16 boot  <- first-stage loader, runs on the GPU
 partition of the SD card
    |
    v
 bootcode.bin loads start.elf into      <- the firmware proper (also GPU code)
 memory and starts it
    |
    v
 start.elf reads config.txt              <- kernel name, load address, and the
    |                                       settings in hardware.md
    v
 start.elf loads kernel.img at the       <- the file LumeOS produces with
 address from config.txt (0x8000)           tools/elf2bin.py
    |
    v
 start.elf loads fixup.dat, sets up the  <- GPU/ARM memory split, clocks
 SDRAM, the ARM clock and the mailboxes,
 and releases the ARM core
    |
    v
 ARM1176JZF-S starts executing at        <- the first instruction of LumeOS:
 0x8000, in ARM state, supervisor mode,     kernel/arch/arm/boot.S
 MMU off, caches off, interrupts masked
```

Key consequences:

* **There is no firmware interface to call.** By the time ARM code runs, the
  GPU has already committed to loading a flat binary at a fixed address. There
  is no ACPI, no device tree *handed over* (unless you ask for one with
  `device_tree=`), and no runtime firmware services beyond the mailbox.
* **The ARM starts in a very specific state.** ARM mode, supervisor mode, MMU
  off, D-cache and I-cache off, interrupts masked, at `0x8000` with the
  registers unspecified.
* **The GPU keeps running.** It owns the memory split, the clocks and the
  mailboxes, and it can reset the board. It never yields: an ARM kernel shares
  the chip with a permanently running real-time GPU firmware.

---

## 2. `config.txt` options LumeOS depends on

The file is in the boot partition and is read by `start.elf` before the kernel.
LumeOS ships `config.txt` in the image; the relevant lines are:

| Option | Value | Effect LumeOS relies on |
| --- | --- | --- |
| `kernel` | `kernel.img` | which file on the card is the ARM payload |
| `kernel_address` | `0x8000` | where it is loaded and entered. LumeOS is linked for exactly this (`kernel/ld/lumeos.ld`: `LMA 0x8000`). The 32-bit default is `0x8000`; saying it explicitly documents the contract |
| `disable_commandline_tags` | `1` | stops the firmware writing an ATAGS block at `0x100`. LumeOS does not read ATAGS; it asks the mailbox for the RAM size |
| `enable_uart` | `1` | required on a Pi Zero W: the firmware enables the primary UART and pins its clock so 115200 8N1 is stable. (On a BT-equipped board the *primary* UART is the mini UART, so this alone is not enough for LumeOS - see the next row.) |
| `dtoverlay=disable-bt` | – | moves the **PL011** to GPIO14/15 instead of the Bluetooth modem. LumeOS drives the PL011 (`kernel/drivers/uart_pl011.c`, base `0x20201000`), so without this the kernel's serial output goes to the Bluetooth chip |
| `uart_2ndstage` | `1` (commented out) | firmware-level early boot messages. The first thing to try when nothing appears on the wire |
| `kernel_old` | *not set* | `1` would load the kernel at `0x0` instead of `0x8000`. LumeOS requires the default |

Not needed, and deliberately absent:

* `device_tree=` / `dtoverlay` for hardware: LumeOS has no FDT parser; it drives
  the peripherals directly by register address.
* `gpu_mem=`: any split works, the kernel asks the mailbox how much RAM it got.
* `hdmi_*` / `framebuffer_*`: there is no framebuffer driver yet.

---

## 3. Addresses, and where they come from

```
0x00000000  ARM RAM (512 MiB on a Zero W, minus the GPU split)
0x00000000  exception vector table when the MMU is off (filled at runtime)
0x00001000  ATAGS area historically written by the firmware (disabled here)
0x00004000  LumeOS level 1 translation table (16 KiB, built by boot.S)
0x00008000  LumeOS kernel image, loaded here by the firmware  <- entry point
0x0000F000  end of the usable kernel image (linker ASSERT enforces this)
0x000F0000  exception vector page (4 KiB; mapped at 0xFFFF0000 by the MMU)
0x20000000  peripherals, ARM view (mapped at 0xF0000000 by the MMU)
0x7E000000  the same peripherals, VideoCore view (what the datasheet prints)
```

Three details worth being precise about:

* **The firmware's peripheral base is `0x7E000000`; the ARM's is
  `0x20000000`.** Both exist at the same time and address the same registers;
  the difference is only which bus (and therefore which alignment/mapping rules)
  a master uses. LumeOS uses the ARM view, so every driver goes through
  `PERIPHERAL_TO_VIRT()` from `kernel/include/lume/hw/bcm2835.h`.
* **The vector page is at physical `0xF0000` and mapped at `0xFFFF0000`.**
  LumeOS runs with high vectors (`SCTLR.V` set), so an exception jumps to a
  virtual address that is *not* where the table physically lives - the MMU has
  to translate it. This is why a fault at `0xffff000c` in the emulator means the
  exception entry itself failed rather than that memory was corrupted.
* **`0x8000` is the load address, `0xC0008000` the link address.** LumeOS is
  linked at the high alias (`KERNEL_VMA_BASE = 0xC0000000`), so the boot stub
  runs at `0x8000` with the MMU off, builds the translation table, enables the
  MMU, and only then branches to `0xC0008000`. The linker script is written so
  that the ELF contains exactly one `PT_LOAD` segment at `0x8000`; a second one
  (for example a `PHDRS`-less layout with an ELF-header segment) would make
  `tools/elf2bin.py` produce a multi-gigabyte file, which is why it checks the
  physical address and fails loudly instead.

---

## 4. The first instruction: `kernel/arch/arm/boot.S`

What the stub does, in order, and why each step is where it is:

1. **Save r0-r2, disable interrupts, invalidate caches.** The firmware leaves
   the machine in an unspecified state.
2. **Build the level 1 translation table at `0x4000`** (16 KiB, 4096 sections).
   The map is deliberately simple:
   * `0x00000000-0x0FFFFFFF` → RAM sections, plus an identity map of the low
     memory the stub is executing from while the MMU is being enabled;
   * `0x20000000-0x21FFFFFF` → peripherals mapped at `0xF0000000` as device
     memory (TEX=0, C=0, B=1), 32 MiB;
   * `0xC0000000-0xCFFFFFFF` → the same 256 MiB of RAM, cached, at the kernel
     alias;
   * `0xFFFF0000` → the 4 KiB vector page at physical `0xF0000`.
3. **Enable the MMU** with caches on (`SCTLR` M/C/I, alignment checking on,
   high vectors on), then **branch to the high alias** and continue there.
4. **Discard the identity mapping** of low memory (except what is still needed)
   and zero `.bss`.
5. **Set up the per-mode stacks**: SVC, IRQ, FIQ, abort and undefined each get
   their own region, with the exported `__stack_*_top` symbols at the *top* of
   each region (stacks grow downwards).
6. **Enable interrupts and jump to `kernel_main(r0 = FDT physical address,
   r1 = kernel load address)`.**

The stub is position independent and uses no stack until the per-mode stacks
exist; the very first lines run at physical `0x8000` and must not touch anything
else.

---

## 5. Assumptions, and what happens if one is wrong

| Assumption | Where | If it is wrong |
| --- | --- | --- |
| The firmware enters at `0x8000` in ARM state, SVC mode, MMU off | `boot.S:1`, `config.txt` | the first instruction is never reached; nothing on serial. This is exactly the class of bug the emulator boot test exists to catch |
| `kernel.img` is loaded at `0x8000` and is the whole kernel | `tools/elf2bin.py --require-paddr 0x8000`, linker ASSERT | a wrong load address makes every absolute branch and literal wrong, usually an instant fault |
| RAM starts at `0x0` and the kernel may use everything above its own image, minus what it reserves | `kernel/arch/arm/memdetect.c`, `main.c` | overwriting firmware or GPU memory hangs the board unpredictably |
| The firmware answers mailbox calls for the clock rate and RAM size | `kernel/drivers/mbox.c` | `uart_init()` falls back to a default clock divisor (which may give a wrong baud rate) and memory detection falls back to a conservative default |
| The PL011 is at `0x20201000` with the firmware-set clock | `uart_pl011.c` | no output. On a Zero W this is also what happens if `dtoverlay=disable-bt` is missing |
| The system timer is a free-running 1 MHz counter with 4 compare registers | `timer.c` | no scheduler tick; the fallback poll would keep the system alive but with poor timing |
| The interrupt controller reports shared IRQs 0-63 through `IRQ_PENDING_1/2` | `irq.c` | interrupts are not dispatched; see the known-bugs list in [roadmap.md](roadmap.md) |
| The GPU firmware does not touch our memory after the hand-off | everywhere | nothing LumeOS can do about it; it is documented behaviour rather than a guarantee |

### The one assumption that is documented but wrong in QEMU

QEMU's BCM2835 model wires the system timer to the interrupt controller's
**GPU IRQ** lines, while the BCM2835 manual describes the timer's compare
channels as interrupt numbers 0-3 in the shared IRQ space that LumeOS's driver
reads. This is a real discrepancy between the datasheet-based design and the
emulator, and it is one of the open items in the boot-failure investigation -
see [roadmap.md](roadmap.md#known-bugs-at-the-current-head). It is a good
illustration of why emulator success is not hardware success: a kernel tuned
only to satisfy QEMU could be wrong on the board.

---

## 6. What the ARM finds when the firmware has finished

| Register/state | Value |
| --- | --- |
| PC | `0x00008000` |
| Mode | Supervisor (SVC), ARM state |
| MMU | off (`SCTLR.M = 0`) |
| Caches | off (`SCTLR.C = 0`, `SCTLR.I = 0`) |
| Interrupts | masked (`CPSR.I = 1`, `CPSR.F = 1`) |
| L1 cache / branch prediction | off |
| r0, r1, r2 | unspecified in general; the firmware's convention is that r0 and r1 carry information for a kernel that asked for it (`kernel_old`, ATAGS). LumeOS treats them as untrusted and only reads `r1` as the load address, otherwise relying on memory detection through the mailbox |
| Stack pointer | not set up by the firmware; LumeOS builds its own. The stub deliberately uses no stack before its per-mode stacks exist |
| `SCTLR.A` (alignment) | LumeOS sets it, and compiles with `-mno-unaligned-access` so the compiler never exploits the "it might work anyway" path |

This is why `boot.S` can be so small: everything it needs, it sets up itself.
The only thing it cannot set up is the physical location of the peripherals and
the memory size, which are SoC properties rather than configuration - those are
constants from the BCM2835 manual and are verified at runtime through the
mailbox where possible.

---

## 7. Recovering a board that does not boot

The Pi's boot chain fails in ways that look identical from the outside
(nothing on screen, nothing on serial). Practical order of attack:

1. Put the card back in a computer and check the boot partition: exactly
   `bootcode.bin`, `start.elf`, `fixup.dat`, `config.txt`, `kernel.img` present.
   `python3 tools/verify_image.py --image build/lumeos-sd.img --kernel
   build/kernel.img --firmware-dir .firmware` validates the image you built.
2. Try the card in a *known-good* Raspberry Pi OS setup first (or re-flash
   Raspberry Pi OS and confirm the card and board are fine). If Raspberry Pi OS
   does not boot, LumeOS never had a chance.
3. Add `uart_2ndstage=1`. If firmware messages appear, the failure is above the
   firmware (in LumeOS); if not, it is below (card, firmware files, power).
4. Re-flash with `dd` and `conv=fsync`, then `sync` - a truncated write produces
   a card that looks plausible and boots nothing.
5. Remember the `disable-bt` trap: on a Zero W, a working kernel can be printing
   perfectly to a chip you cannot see.

None of the above is LumeOS-specific, which is exactly the point: when the
kernel does not appear on the wire, the majority of the causes are *below* the
kernel.
