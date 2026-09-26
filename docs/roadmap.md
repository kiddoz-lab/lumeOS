# LumeOS roadmap

Where the project is, what comes next, and what "done" means for each step.
Statuses are kept strict - *works* means a test that actually ran passed, not
"the code looks right".

| | |
| --- | --- |
| Today | a static ARM ELF loads and runs in user mode: `init` prints from user mode, makes syscalls and exits with a status the kernel reaps (CI-enforced, `59/59` self tests) |
| Next | validate the same image on a real Pi Zero W, then give userspace a filesystem to load programs from |
| Then | the syscall surface a real libc needs - `auxv`, `mmap2`, `openat`, `stat64`, signals, `clone`, `futex` - and then a real ARM Linux binary |

Legend: ✅ works and is covered by a test that ran - and unless a row says
otherwise that test is the *emulator*, not a Raspberry Pi · 🟡 written, not yet
observed working, or observed but not measured · ⛔ not started.

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

## Milestone 1 - boot to the boot markers under QEMU 🟡 (emulator part done)

The kernel must reach all four markers in the emulator before anything else is
worth building on top: an unreliable foundation makes every later bug
ambiguous.

| Item | State |
| --- | --- |
| Boot stub: MMU, caches, high vectors, per-mode stacks | ✅ reaches C code and the shell prompt in QEMU; **unverified on hardware** |
| PL011 console at 115200 8N1 | ✅ prints in QEMU; **unverified on hardware** |
| Physical memory manager, kernel heap | ✅ covered by self tests that pass on every boot (emulated) |
| MMU section/page mapping, device memory attributes | ✅ idem, including the translation API's unmapped case |
| System timer (1 MHz counter, compare channel 3, 100 Hz tick) | ✅ idem; the tick counter advances and the IRQ count is sane (74 exceptions in a ten-second run, versus 1.6 million before the fix) |
| Interrupt controller, IRQ dispatch | ✅ self-checking handlers and unclaimed-interrupt reporting; the storm is gone and the last exception of a run is an ordinary IRQ |
| Preemptive scheduler, threads | 🟡 context switch, wait queues and the idle loop all execute; starvation and fairness are unmeasured |
| Kernel self tests (44 checks) | ✅ `44/44` asserted by the emulator test on every CI run |
| Kernel shell (`help`, `mem`, `ps`, `time`, `irq`, `echo`, `reboot`, `halt`, `version`) | 🟡 the prompt appears in QEMU (and now *after* init has run and exited); no command has been typed into it yet |

### Fixed since this milestone started

The two bugs that used to sit at the top of this list are closed, and the
evidence is in [testing.md](testing.md#what-the-emulator-run-currently-proves):

1. **The interrupt storm was a level-held source, not bad routing.** The PL011
   handler acknowledged only the RX and receive-timeout bits, so an interrupt
   caused purely by an error condition (overrun, break, framing, parity) was
   returned from without acknowledging anything, leaving the line asserted and
   re-entering the handler forever - 1,646,273 exceptions in ten seconds. The
   handler now clears every bit the device reports, and lines aggregated into
   the controller's *basic pending* register register themselves as
   self-checking. A run now counts 74 exceptions, and the prefetch abort at
   `IFAR 0xffff000c` that the storm ended in is gone with it.
2. **`vmm_translate()`'s 0-sentinel collided with physical address 0**, so
   "unmapped" and "the kernel alias at 0xC0000000" were indistinguishable and
   one self test could never pass. It now returns `0`/`-1` and reports the
   physical address through an out-parameter.

### Known bugs at the current head

These are open and real. None of them is visible in the emulator run today,
which is itself a limitation: they are all paths the QEMU boot does not reach.

1. **The `__restore_regs` path and the r8/r9 save order** in
   `kernel/arch/arm/vectors.S` are wrong for a context switch that returns to a
   user bank. Harmless while everything runs in SVC mode - which is why nothing
   fails today - and must be fixed before the first userspace entry.
2. **The trap frame is not yet validated in bulk.** The self tests cover the
   memory manager, MMU, timer, string library and division helpers, but not "an
   IRQ arrives, the handler runs, the frame is restored faithfully". That is the
   next self test worth writing, because every userspace bug will otherwise look
   like an unexplained register corruption.
3. **The scheduler runs, but nothing measures it.** Preemption, wait queues and
   the idle loop execute; there is no test that a thread actually gets the CPU
   after another one spins, nor any accounting of idle time.
4. **The emulator's interrupt model differs from the manual** in how the timer
   and the UART reach the interrupt controller (QEMU wires them to GPU IRQ
   lines; the manual describes the shared IRQ numbers the driver uses). The
   kernel services both paths and counts unclaimed interrupts, so a mismatch
   will show up as a counter rather than as silence - but it will only be
   settled on hardware, which is one more reason to keep the boot honest.

### Definition of done for milestone 1

* ✅ `make test-qemu` passes with `--required` (all four markers, plus
  `selftest: N/N checks passed`) - done at commit `a7e8698`, CI run
  `35755471382`;
* ✅ the CI step lost `continue-on-error` in the same commit and now fails the
  build on a boot regression;
* ⛔ **the same image booted on a real Pi Zero W**, with the boot markers
  observed on the serial console and the board, card, image SHA-256 and terminal
  settings recorded in [testing.md](testing.md). This is the part of milestone 1
  that is still open, and it cannot be closed by anything in this repository -
  only by someone with the board.

## Milestone 3 - userspace: the first ARM Linux binary 🟡 (started)

The first half is done and asserted by CI: the kernel loads a static ARM ELF,
enters user mode, dispatches syscalls and reaps the process.

| Item | State |
| --- | --- |
| ELF32 `EM_ARM` `ET_EXEC` loader | ✅ maps `PT_LOAD` segments with their real permissions, zeroes `bss`, handles pages shared by two segments, cleans/invalidates caches; 7 self tests |
| Initial stack (`argc`/`argv`/`envp`) | ✅ built through the *new* address space (see `copy_to_user_as`), `sp` 16-byte aligned, strings above the tables; 26 self tests, and a host test (`tests/host/test_ustack_layout.py`) that runs the layout arithmetic without an emulator |
| Syscall entry and dispatch | ✅ `svc #0`, `r7`, `r0-r5`, Linux error convention, `-ENOSYS` + one log line per unimplemented number |
| `write`, `read`, `exit`, `exit_group`, `getpid`, `getuid`/`euid`, `getgid`/`egid`, `brk`, `uname`, `wait4`, `set_tls` | ✅ |
| A program that runs and prints through `write(2)` | ✅ `userspace/init`, built by `make userspace`, embedded by `tools/embed_user.py`, asserted marker by marker in QEMU |
| Process reaping and thread-slot reuse | ✅ a kernel watchdog waits with `wait4` semantics and reports the status |
| `auxv` (with honest `AT_HWCAP`) | ✅ 17 pairs + `AT_NULL`, `AT_HWCAP` = `0x00008097` with no FP bit (the static assert in `auxv.h` and the check in the program both say so); `AT_RANDOM` is documented as *not* cryptographic until the BCM2835 RNG is driven |
| `mmap2`, `mprotect`, `munmap` | ⛔ |
| Filesystem + `openat`/`stat64`/`getdents64` | ⛔ the VFS is interfaces only; this is what turns the embedded blob into `/bin/init` on a card |
| Signals, `clone`, `futex`, pipes, `clock_gettime` | ⛔ |

### Definition of done for the first half of milestone 3

* ✅ `userspace/init` runs in ARM user mode and its own output appears in the
  emulator's serial log;
* ✅ the same output is a **required** marker in the QEMU test, so breaking user
  mode breaks the build;
* ⛔ that run happens on a real Pi Zero W (milestone 2's open item).

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

1. **A static musl binary, then what it asks for.** The auxiliary vector is in
   place (`docs/userspace.md` lists the pairs), so the next step is to build a
   static `hello world` with a real C library, run it, and implement what it
   turns out to need. The expected list is `mmap2`, `mprotect`, `munmap`,
   `clock_gettime` and `rt_sigaction`; the honest way to order them is the log
   line the kernel already prints for each unimplemented number. Its startup
   will also exercise the initial stack in a way our own program cannot.
2. **A filesystem, so the next program is not embedded in the kernel.** A
   read-only FAT16 reader on the SD card plus path lookup in the VFS is the
   smallest thing that turns `init` from a blob in `.rodata` into `/bin/init`,
   and it is what makes `openat`/`stat64`/`getdents64` implementable at all.
3. **Extend the exception probe to the IRQ path.** The same trick as
   `trapprobe.S` works for an interrupt: arm a one-shot timer compare, spin with
   a known register pattern, and check the frame the handler saw and the
   registers that came back. That covers the nested case (an IRQ arriving while
   the kernel is inside `__restore_regs` or at a wait point), which nothing
   tests today.
4. **Make `make test-qemu` runnable locally, and keep the diagnosis alive.** The
   diagnosis step only fires on failure today; running it behind `--diagnose`
   every Nth CI run keeps the "quiet run" claim measured instead of assumed.
5. **A hardware boot** (milestone 2). It can jump the queue at any point: a bug
   found on a board changes what everything above has to look like.

---

## How this file is kept honest

Three rules, applied whenever a milestone row changes:

1. **A tick means a test that ran.** "✅" never means "the code looks right" - it
   means a named CI run or a documented local command produced the result, and
   where it matters the run or commit is named in the row itself.
2. **Every feature appears with what it does not cover.** `brk` says it refuses
   to grow into the mmap region; the ELF loader says it refuses PIE by name; the
   console device says it has no terminal ioctls. A row without a limitation is
   a row that has not been examined.
3. **Nothing is marked done because it is nearly done.** The trap-frame probe
   spent exactly one commit in this file described as "written, emulator run
   pending", with the reason (a GitHub credential problem on the machine that
   wrote it) - and the next run turned it into a ✅ with the commit that showed
   it. That is the pattern: state what is unverified and why, then close it with
   evidence.
