# LumeOS roadmap

Where the project is, what comes next, and what "done" means for each step.
Statuses are kept strict - *works* means a test that actually ran passed, not
"the code looks right".

| | |
| --- | --- |
| Today | bring-up: the kernel builds, is ISA-gated, and runs far enough under QEMU to reach the scheduler (details below) |
| Next | fix the interrupt path so the kernel reaches its boot markers in the emulator, then validate on a real Pi Zero W |
| Then | userspace: ELF loader, syscalls, `init`, a shell - and the first real ARM Linux binary |

Legend: ✅ works and is covered by a test that ran · 🟡 written, not yet observed
working · ⛔ not started.

---

## Milestone 0 - the project itself ✅

| Item | State |
| --- | --- |
| Source tree, build system, linker script, startup | ✅ builds with GNU arm-none-eabi and with `zig cc` (see [building.md](building.md)) |
| Flat `kernel.img` at `0x8000` | ✅ produced by `tools/elf2bin.py`, checked for the physical address |
| Bootable SD image (MBR + FAT16 + firmware + kernel) | ✅ built and structurally verified by `tools/verify_image.py` in CI |
| ARMv6 instruction gate, EABI gate | ✅ `tools/check_isa.py`, `tools/check_abi.py`, both with host tests |
| Host unit tests | ✅ `make test-host` (printf, string, division cores, image tooling, both gates) |
| Documentation | ✅ this document set |

## Milestone 1 - boot to the boot markers under QEMU 🟡

The kernel must reach all four markers in the emulator before anything else is
worth building on top: an unreliable foundation makes every later bug
ambiguous.

| Item | State |
| --- | --- |
| Boot stub: MMU, caches, high vectors, per-mode stacks | 🟡 executes; no faulting reported in the stub itself |
| PL011 console at 115200 8N1 | 🟡 prints in QEMU; **unverified on hardware** |
| Physical memory manager, kernel heap | 🟡 self tests exist and run on every boot; results not yet observed |
| MMU section/page mapping, device memory attributes | 🟡 idem |
| System timer (1 MHz counter, compare channel 3, 100 Hz tick) | 🟡 idem |
| Interrupt controller, IRQ dispatch | 🟡 idem, and this is where the boot currently breaks |
| Preemptive scheduler, threads | 🟡 reaches the scheduler's context switch in the trace |
| Kernel self tests (43 checks) | 🟡 run on every boot and print a summary line the emulator greps for |
| Kernel shell (`help`, `mem`, `ps`, `time`, `irq`, `echo`, `reboot`, `halt`, `version`) | 🟡 written, never interacted with |

### Known bugs at the current head

These are open, reproducible from the emulator trace, and not hidden behind
"informational" language:

1. **Interrupt storm + a fault inside the exception vector.** The emulator
   counts well over a million IRQs in a ten-second run; the overwhelming
   majority return to `uart_rx_ready+0x14`, i.e. the PL011 receive interrupt is
   being taken continuously. The last exception is a prefetch abort with
   `IFSR 0x5` (section translation fault) at `IFAR 0xffff000c`, inside the
   high-vector page. Two things are therefore wrong or at least suspect:
   the RX interrupt is firing without data to read, and the vector page's
   `Prefetch Abort` entry is not executable through the mapping the kernel
   installed. Neither can be confirmed from the trace alone - the next step is
   to check the vector page's page-table entry and the `PL011_MIS`/`ICR`
   handling against the datasheet.
2. **The PL011 interrupt routing is a documented-vs-emulator discrepancy.**
   QEMU wires the system timer and the UART to the interrupt controller's GPU
   IRQ lines; the BCM2835 manual describes the shared IRQ numbers the kernel
   driver uses. The kernel's timer driver already falls back to polling the
   counter (and reports how many ticks came from the fallback), but the UART
   path has no such fallback, which may be exactly why one IRQ source dominates.
3. **The `__restore_regs` path and the r8/r9 save order** in
   `kernel/arch/arm/vectors.S` are wrong for a context switch that returns to a
   user bank. Harmless while everything runs in SVC mode; must be fixed before
   the first userspace entry.
4. **The trap frame is not yet validated in bulk.** The self tests cover the
   memory manager, the timer, the string library and the division helpers, but
   not "an IRQ arrives, the handler runs, the frame is restored faithfully".

### Definition of done for milestone 1

* `make test-qemu` passes with `--required` (all four markers, plus
  `selftest: N/N checks passed`);
* the CI step loses `continue-on-error` and becomes a real gate;
* the same image is booted on a real Pi Zero W and the boot markers are observed
  on the serial console, with the board, card, image SHA-256 and terminal
  settings recorded in [testing.md](testing.md).

## Milestone 2 - a real hardware boot ⛔

* Serial console validated on hardware (including `dtoverlay=disable-bt`, which
  the emulator cannot test at all).
* Timer and interrupt behaviour validated with real timing (a 100 Hz tick that is
  actually 100 Hz, not QEMU's virtual clock).
* Memory detection cross-checked against the actual board (mailbox
  `GET_ARM_MEMORY` versus the memory map in [boot-pi.md](boot-pi.md)).
* The first hardware bug - there will be one - diagnosed from the kernel's own
  fault dumps and written up in [research-notes.md](research-notes.md).

## Milestone 3 - userspace entry ⛔

The single biggest step, and the one that turns this from a kernel into an
operating system.

| Item | Notes |
| --- | --- |
| Per-process address spaces | L1 tables per process, kernel mapping shared, `mmu.c` already builds the kernel's |
| ELF32 loader | `ET_EXEC`, static, one or more `PT_LOAD`; see [userspace.md](userspace.md#4-elf-loading) |
| `argv`/`envp`/`auxv` stack construction | exact Linux layout; `AT_HWCAP` must describe an ARMv6 CPU with no VFP |
| SVC entry path with a real syscall table | `do_syscall()` today reports and kills; it becomes a dispatcher |
| First syscalls | `write`, `exit`, `exit_group`, `brk`, `mmap2`, `close`, `read`, `open`/`openat` |
| `src/userspace/` (init + a tiny test program) | statically linked, built by a real `make userspace` target |
| `make test-userspace` | runs the test program under QEMU and greps PASS/FAIL over serial |

**Definition of done:** a statically linked program that LumeOS did not compile
(an independent toolchain producing a standard ARM Linux ELF) prints to the
serial console and exits cleanly, and the same program runs on hardware.

The `make help` text currently advertises a `userspace` target that does not
exist. It will exist when the directory does; until then the help text is wrong
and that is a bug to fix in the same commit that adds the directory.

## Milestone 4 - enough of Linux to run a shell ⛔

| Item | Notes |
| --- | --- |
| VFS with nodes and mounts | `kernel/include/lume/fs.h` defines the interfaces; the implementation is the work |
| RAM filesystem + device nodes | `/dev/console`, `/dev/null`, `/dev/tty`, `/dev/kmsg`, `/dev/urandom` |
| `/proc` | at least `self/maps`, `self/auxv`, `self/status`, `meminfo`, `uptime` |
| FAT32 on the SD card | the boot partition is FAT16 already; the EMMC/Arasan controller driver comes first |
| Processes and `fork`/`execve`/`wait4` | the kernel-side structures exist; the user-visible semantics do not |
| Signals | `rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn` with a correct interrupted context |
| Pipes and `dup`/`fcntl` | enough to build a pipeline in a shell |
| `futex`, `clone` with TLS | required by any modern libc |
| A LumeOS-native shell (`/bin/lsh`) | the kernel shell (`kshell.c`) is a bring-up tool, not a userspace shell, and stays separate |

**Definition of done:** a real, unmodified ARM Linux `busybox` shell runs
`ls`/`cat`/`echo` over a LumeOS filesystem.

## Milestone 5 - real Bash ⛔

The stated long-term target. "Real" means: a `bash` binary built by an
independent ARM Linux toolchain, statically linked, running unmodified. Not a
program of ours named `bash`.

The gaps that a shell exposes, in the order they usually bite:

1. `fork`/`exec` honesty (copy-on-write or at least correct copy semantics);
2. `waitpid` with status encoding, job control signals (`SIGTSTP`, `SIGCONT`),
   process groups and sessions;
3. `signal` semantics under a real load (restarting syscalls, `EINTR`);
4. `ioctl` on a terminal (termios) so interactive line editing behaves;
5. file descriptor inheritance and `close-on-exec`;
6. `getcwd`, `chdir`, `readlink`, `stat` corner cases;
7. `time`/date formatting (`localtime`, so `/etc/localtime` or `TZ`);
8. enough "reasonable" behaviour in the face of missing features that the shell
   reports an error instead of dying (a shell that cannot run `ls` must still be
   able to print a prompt).

## Milestone 6 - a usable little machine ⛔

| Area | Plan |
| --- | --- |
| Framebuffer | mailbox `SET_PHYS`/`SET_VIRT`/`DEPTH`/`ALLOCATE`, then a console on the framebuffer and an 8x16 font |
| Keyboard | USB (DWC OTG controller + HID class) - the piece with the most unknowns on this SoC |
| Touchscreen | the 3.5" SPI panels are a `fbtft`-style controller over SPI plus a resistive touch controller on SPI/GPIO |
| Storage | EMMC/Arasan SD controller, then a filesystem that can share the card with the firmware's FAT partition |
| Networking | the BCM43438 over SDIO/SDIO-over-GSPI plus an 802.11 stack, then a TCP/IP stack; realistically: move the WiFi to a later phase after the wired-less board has serial and SD working |
| Time | no RTC on the board; set the wall clock from the network when there is one, and keep `CLOCK_MONOTONIC` honest meanwhile |

Explicitly **not** on the roadmap: porting Chromium, V8, X11, Wayland, GNOME
or KDE. If a graphical environment ever appears it will be a small native one
that fits in the RAM of a 32-bit machine with no VFP.

---

## How this roadmap is kept honest

Three rules, enforced by convention and by CI:

1. **No milestone is moved to "done" without a test that ran.** "It builds" is
   not a test result, and neither is "it works in QEMU" for a hardware claim.
2. **Documentation states the failure mode as well as the feature.** Every
   subsystem in these documents appears with what it does *not* cover.
3. **The emulator is not the board.** Every conclusion drawn from QEMU is
   labelled as such, and every hardware-only assumption is listed explicitly in
   [boot-pi.md](boot-pi.md#5-assumptions-and-what-happens-if-one-is-wrong).

## Near-term work queue

In the order the next commits should happen:

1. Fix the exception/interrupt path so the QEMU boot test reaches its markers
   (milestone 1). The trace says: RX interrupt storm, then a fault at
   `0xffff000c`; check the vector page mapping and the PL011 `MIS`/`ICR` logic,
   and check the timer's IRQ routing against QEMU's model.
2. Make the QEMU step blocking (`continue-on-error` removed) once it passes.
3. Turn the `userspace` target into a real target with a first static test
   program, and make the `make help` text true again.
4. Then milestone 3 in order: address spaces, ELF loader, syscall table,
   `write`/`exit`/`brk`/`mmap2`.
