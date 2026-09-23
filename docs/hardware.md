# Running LumeOS on a Raspberry Pi Zero W

This is the practical document: what board, what card, what cable, what to type,
what you should see, and what to do when you do not see it. The companion
document [boot-pi.md](boot-pi.md) explains *why* the boot chain behaves this way.

> **Status warning.** LumeOS has never been run on a real Pi Zero W. Nothing on
> this page has been validated on hardware; it is the procedure derived from the
> public documentation and from the emulator work, written down so that the
> first hardware session is a checklist rather than an experiment. Treat every
> "you should see" as a prediction.

---

## 1. The board

| | Raspberry Pi Zero W (the 2017 model) |
| --- | --- |
| SoC | Broadcom BCM2835 |
| CPU | ARM1176JZF-S, ARMv6KZ, single core, 1 GHz |
| RAM | 512 MiB, shared with the VideoCore GPU |
| Storage | microSD / microSDHC |
| Video | mini-HDMI |
| USB | one micro-USB OTG port |
| Wireless | 2.4 GHz WiFi + Bluetooth 4.1 (BCM43438) |
| UARTs | PL011 (UART0) and the mini UART (UART1) |
| GPIO header | 40-pin, unpopulated |

Two things about this model matter for LumeOS specifically:

1. **The PL011 UART is shared with the Bluetooth modem on this board.** Only
   the mini UART is wired to GPIO14/15 by default. LumeOS drives the *PL011*
   (`kernel/drivers/uart_pl011.c`, base `0x20201000`), so the boot
   configuration must hand the PL011 to the GPIO header - that is what
   `dtoverlay=disable-bt` in `config.txt` does. Without it, the kernel's output
   goes to the Bluetooth chip, which is also the classic "my Pi prints nothing
   on the serial console" trap on every BT-equipped model.
2. **It has no Ethernet and one USB port.** There is no network fallback and no
   way to attach both a keyboard and something else without a hub, so the serial
   console is not a convenience - it is the only practical interface while
   bring-up is in progress.

The board also has no power switch and no RTC. LumeOS is explicit about both:
`halt` stops the CPU instead of pretending it can cut power, and the wall clock
starts unset (`time_unset` prints "no RTC on this board").

---

## 2. What you need

| Item | Notes |
| --- | --- |
| Raspberry Pi Zero W | the ARM1176/BCM2835 target. A Pi Zero *2* W or any Pi 3/4/5 is a **different SoC** (ARMv7/ARMv8) and cannot run this kernel |
| microSD card, 8 GiB or larger | the image is 64 MiB, so size is irrelevant; class 4 or better and a known-good card is not |
| micro-USB power supply, 5 V 2 A | plus a micro-USB OTG adapter if you want USB devices |
| USB-to-serial adapter, **3.3 V** | CP2102, CH340, FT232RL or an official Pi debug probe. A 5 V TTL adapter can damage the SoC - check before connecting |
| 3 jumper wires | TX, RX, GND |
| mini-HDMI adapter + cable | optional, only useful once the framebuffer driver exists (it does not yet) |

On power: the Pi Zero W's micro-USB port is power-only, the second micro-USB port
is the OTG/USB port. Both are micro-USB, and plugging power into the wrong one
(connecting a host PC to the OTG port while powered) is harmless but confusing -
the board will not boot if it is not supplied through the correct port.

---

## 3. Flash the image

```sh
make firmware      # once: fetch bootcode.bin, start.elf, fixup.dat
make image         # -> build/lumeos-sd.img (with verification)
lsblk              # identify the card - CHECK the device name
sudo dd if=build/lumeos-sd.img of=/dev/sdX bs=4M conv=fsync status=progress
sync
```

Or download the `lumeos-sd-image` artifact from a CI run instead of building it;
the same `tools/verify_image.py` check ran against it during the build, and the
SHA-256 is in the notice annotation of that run.

The image contains, in a FAT16 boot partition:

| File | Purpose |
| --- | --- |
| `BOOTCODE.BIN` | first-stage loader the GPU runs from ROM |
| `START.ELF` | the firmware proper: reads `config.txt`, loads the kernel |
| `FIXUP.DAT` | firmware fixups, required by `start.elf` |
| `CONFIG.TXT` | LumeOS's boot configuration (see below) |
| `KERNEL.IMG` | LumeOS, flat binary, to be loaded at `0x8000` |

`config.txt` as shipped:

```
kernel=kernel.img
kernel_address=0x8000
disable_commandline_tags=1
```

For the serial console to work on a Pi Zero W, two more lines are needed:

```
enable_uart=1
dtoverlay=disable-bt
```

* `enable_uart=1` makes the firmware enable and clock the primary UART and keep
  its clock fixed, which is what makes 115200 8N1 stable.
* `dtoverlay=disable-bt` moves the PL011 back to GPIO14/15 (and turns Bluetooth
  off). LumeOS talks to the PL011, so this is not optional on this board.
* `disable_commandline_tags=1` stops the firmware writing ATAGS into low memory.
  LumeOS does not read ATAGS: it asks the firmware for the RAM size over the
  mailbox property interface instead (`GET_ARM_MEMORY`).
* `#uart_2ndstage=1` (commented out) makes the firmware print its own early boot
  progress. That is the single most useful debugging switch if the board does
  not seem to boot at all: it distinguishes "the firmware never started" from
  "the kernel started and died".

If you edit `config.txt` after flashing, the card is plain FAT16 - mount it and
edit the file in place. No re-flash needed.

---

## 4. Wire the serial console

```
  Pi Zero W                          USB-to-serial adapter
  ---------                          ---------------------
  GPIO14  (pin 8,  TXD)  ------>     RXD
  GPIO15  (pin 10, RXD)  <------     TXD
  GND     (pin 6)        -------     GND
```

Rules that save an hour of confusion:

* **TX to RX, RX to TX.** Both sides transmit on their own TX pin.
* **Ground is mandatory.** Without it you get noise or nothing.
* **Never connect the adapter's VCC/5 V pin.** The board is powered over USB;
  feeding 5 V into the header while USB is connected is at best confusing and at
  worst damaging. The adapter must be a 3.3 V one.
* Do not connect the adapter's 3.3 V pin to the Pi's 3.3 V pin either.

Terminal settings: **115200 baud, 8 data bits, no parity, 1 stop bit, no flow
control**.

```sh
# Linux/macOS
screen /dev/ttyUSB0 115200        # or /dev/tty.usbserial-* on macOS
# Ctrl-A k to quit screen

# or
picocom -b 115200 /dev/ttyUSB0
minicom -b 115200 -D /dev/ttyUSB0
```

Then insert the card and apply power. Nothing is written to the card, so a
crash cannot damage the image - just power-cycle.

---

## 5. What you should see (prediction, not a report)

The kernel is designed to announce itself in ordered stages, each one printed
before the next begins. That ordering is the whole point: the last line you see
tells you where the boot stopped.

```
LumeOS 0.1.0 (armv6kz) -- an operating system for the Raspberry Pi Zero W
kernel: entered at virtual 0xc00081xx, loaded at physical 0x00008000, image ends at 0x0002b000
memory: 448 MiB RAM at 0x00000000, ... KiB free after reservations
selftest: running kernel self tests
selftest: N/N checks passed
LumeOS: boot complete, ... KiB free, ... timer ticks
init: loading the embedded NNNNN-byte init image (entry 0x00010000, sha256 ...)
init: entering user mode at 0x00010000 on stack 0xbdffffxx (1 segments, 1 pages)
init: hello from user mode
init: pid 1, argc 1
init: argv[0] is "/bin/init"
init: this line went to file descriptor 2
init: exiting with status 0
init: pid 1 exited with status 0 (exit code 0)
main: entering the idle loop
```

(The exact wording comes from `kernel/kernel/main.c` and
`kernel/kernel/selftest.c`; `LumeOS: boot complete` and
`main: entering the idle loop` are the markers the emulator test greps for, so
they are stable by construction, and so are the `init:` lines - the emulator
test requires every one of them. The check count grows with the kernel (59 at
the last run that was read back) and it is asserted too, so the number in a real
boot log can be compared directly against what CI saw.

The predictions above are now backed by emulator evidence rather than only by
reading the code: the boot reaches all four markers, `59/59` self tests pass, the
user program's own output appears and the shell prompt follows it under QEMU (see
[testing.md](testing.md#what-the-emulator-run-currently-proves)). The lines
between `selftest:` and `main: entering the idle loop` are the ones that say the
hand-off worked: if they are present on your board, LumeOS has run a 32-bit ARM
Linux ELF in user mode on real hardware. What is still
unproven on this page is anything that depends on the firmware, the card, the
wiring, the clocks or the board. If you are the first person to run this on
hardware, that is the open question, not the kernel's internal logic.

Then a prompt from the kernel shell (`help` lists `mem`, `ps`, `time`, `irq`,
`echo`, `reboot`, `halt`, `version`):

```
lume>
```

Realistically, the RAM figure depends on the GPU memory split (the firmware
keeps some for the GPU; a Pi Zero W typically reports ~448 MiB to the ARM with
the default 64 MiB split), and the free-page figure depends on what the firwmare
reserves. Exact numbers are not something this project can promise before
running it.

If the boot hangs, the last line printed is the evidence: silence after the
banner means the failure is in whichever subsystem prints next (memory
detection, the timer, the interrupt controller), while nothing at all means the
failure is in `boot.S`, in the MMU setup or in the UART clock query - all of
which run before the first line. What the kernel has actually been observed to
do under the emulator - and how that observation is produced - is in
[testing.md](testing.md#what-the-emulator-run-currently-proves).

---

## 6. Troubleshooting

### No output at all

Work through this in order; each step eliminates one layer.

1. **Is the firmware running?** Uncomment `uart_2ndstage=1` in `config.txt`. If
   you still see nothing, the problem is before the kernel: card, firmware
   files, or the adapter wiring. If you now see firmware messages, the kernel is
   the one failing to speak.
2. **Is the adapter right way round?** Swap TX/RX. This is the most common cause
   by far. Check the adapter is 3.3 V.
3. **Is the right UART on the header?** On a Pi Zero W, without
   `dtoverlay=disable-bt`, GPIO14/15 carry the *mini UART*, and LumeOS drives
   the PL011. Add the overlay.
4. **Is `enable_uart=1` present?** Without it the firmware may leave the UART
   unclocked; LumeOS asks for the clock rate over the mailbox and will fall back
   to a default divisor, which can give an unreadable baud rate.
5. **Is the card actually booting?** The green ACT LED blinks during firmware
   reads. No blink at all means the firmware is not starting: re-flash, check the
   three firmware files are present with exactly those names in the boot
   partition (the FAT16 image stores them upper-case; that is fine).
6. **Is the card FAT16 as built?** `python3 tools/verify_image.py --image
   build/lumeos-sd.img --kernel build/kernel.img --firmware-dir .firmware`
   re-checks the image you flashed.

### Garbage on the serial line

Baud mismatch, an ungrounded adapter, or a 5 V adapter. Confirm 115200 8N1 and
the ground wire. If it is one character of noise per boot, that is the adapter
picking up the power-on glitch; it is harmless.

### Output starts and then stops mid-line

The kernel died in the middle of that print. Note the last complete line, power
cycle, and compare with the ordered list in §5 to find the stage. Serial output
is not buffered by the kernel, so nothing is lost in a buffer.

### The board boots, then resets in a loop

If `reboot` was typed (or the watchdog was poked) the SoC resets on purpose and
the boot repeats. Otherwise, repeated resets usually mean an exception that the
kernel turns into a panic plus a watchdog that the firmware had already armed.
LumeOS does not arm the watchdog at boot.

### USB keyboard does not work

It is not implemented. There is no USB driver, no framebuffer, no storage and no
WiFi driver yet. The serial console is the only interface that exists - see
[roadmap.md](roadmap.md) for the order in which the rest is planned.

---

## 7. Diagnostic value of the LED and of JTAG

* **ACT LED (green)**: activity on the SD card. Blinking during boot is the
  firmware reading `start.elf` and `kernel.img`. It is not controlled by LumeOS
  and says nothing about the kernel after the hand-off.
* **PWR LED (red)**: power only. On the Pi Zero W it is hard-wired and cannot
  be software-controlled.
* **JTAG**: the ARM1176 has a JTAG port routed to the (unpopulated) GPIO header
  on some boards, but enabling it requires the firmware's `enable_jtag_gpio`
  and a proper adapter. It is not needed for bring-up: serial output plus the
  kernel's own fault dumps carry enough information.

---

## 8. What LumeOS can and cannot use on this board today

| Hardware | Driver | State |
| --- | --- | --- |
| PL011 UART | `kernel/drivers/uart_pl011.c` | written; prints in QEMU; **unverified on hardware** |
| System timer | `kernel/arch/arm/timer.c` | written; **unverified on hardware** |
| Interrupt controller | `kernel/arch/arm/irq.c` | written; **unverified on hardware** |
| GPIO | `kernel/drivers/gpio.c` | written; used for the serial pins; **unverified** |
| VideoCore mailbox | `kernel/drivers/mbox.c` | written; used for the UART clock and RAM size; **unverified** |
| Framebuffer (HDMI / 3.5" SPI screen) | – | **not implemented** |
| USB (keyboard, hub, storage) | – | **not implemented** (needs the DWC USB controller) |
| microSD controller (EMMC/Arasan) | – | **not implemented** (the board boots from the card, but LumeOS cannot read it yet) |
| WiFi / Bluetooth (BCM43438 over SDIO/UART) | – | **not implemented**, and out of scope until the kernel has a filesystem and a network stack |
| RTC | – (none on the board) | wall clock starts unset, settable in software |
