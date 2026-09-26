# Testing LumeOS

LumeOS is tested at four levels, and the distinction between them is kept
explicit everywhere in this project:

| Level | Where | Runs on | Proves |
| --- | --- | --- | --- |
| 1. Static gates | `make kernel` | your machine | what the *machine code* is: legal ARMv6, EABI helpers matching their callers |
| 2. Host unit tests | `make test-host` | your machine | the portable logic: printf, string library, division cores, image tooling |
| 3. Emulated boot test | `make test-qemu` | QEMU (`raspi0`) | that the kernel actually runs, brings up the MMU and drivers, and prints its markers |
| 4. Hardware | a real Pi Zero W | the board | everything else, and the only level that can validate the boot chain, timing and the real peripherals |

**Level 4 does not exist yet.** Nothing in this repository has ever run on a
Raspberry Pi. Every statement below about hardware behaviour is a statement
about the specification the code follows, not about an observed result.

---

## 1. Static gates

These run automatically as part of `make kernel` and fail the build (see
[building.md](building.md#4-the-static-gates-every-build-runs) for the full
description):

```sh
make kernel CHECK_ISA_FLAGS="--require-attributes --require-capstone"
```

* `tools/check_isa.py` - every instruction in every executable section is
  ARMv6KZ-legal, and the build attributes say `v6`/`v6K`/`v6KZ`.
* `tools/check_abi.py` - the `__aeabi_*` division helpers use the fixed rtabi
  register layout, and no call site expects the compiler's struct-return
  convention from them.

What they do **not** prove: anything about behaviour. A kernel can be perfectly
legal ARMv6 and still take a data abort on its first register write (it did).

### The ISA gate has its own test

`tests/host/test_tools.py::CheckIsaTests` feeds hand-encoded instruction words
to the checker and asserts that ARMv7-only instructions are reported, that
legal ARMv6 equivalents are not, and that the extend-and-add forms
(`sxtab`/`uxtab`/...) are accepted - they are ARMv6, and rejecting them made the
gate fail on its own compiler output. The ABI gate has an equivalent test class
with hand-encoded prologues: one sret-shaped helper that must be rejected, one
rtabi-shaped helper that must be accepted, a `stm r0` variant, and a caller that
dereferences `r0` after a call.

---

## 2. Host unit tests

```sh
make test-host          # python3 -m unittest discover -s tests/host
```

These compile parts of the kernel for the **host** CPU and run them, so they
need no ARM toolchain beyond a host C compiler (`cc`/`gcc`/`clang`).

### `test_kernel_lib.py` - the portable kernel sources

Compiles `tests/host/ktest_main.c` together with

| Source | What it covers |
| --- | --- |
| `kernel/kernel/printf.c` | the whole `ksnprintf` family: `%d %u %x %s %c %p`, padding, width, `%llu`/`%lld`, a null `%s`, truncation into a small buffer |
| `kernel/kernel/string.c` | `strlen`, `strcmp`, `strcat`, `strcpy`, `memcmp`, `memchr` (it returns an address, not an offset), overlapping `memmove`, `strtoul` (including its "characters consumed" return) |
| `kernel/kernel/divmod.c` | 32- and 64-bit division and remainder, signed cases, the `0xC0000000_00000000 / 2` top-bit case, and the zero-divisor hook |

The test binary prints `ktest: N checks, M failures`; the test fails unless the
exit code is zero *and* the summary contains `0 failures`. A skipped test (no
host compiler) is reported as a skip, not a pass.

Note that `ktest_main.c` deliberately does **not** include `<string.h>`: every
call must resolve to `kernel/kernel/string.c`, otherwise the test would be
checking glibc.

### `test_ustack_layout.py` - the initial stack the kernel builds

Compiles `tests/host/ustack_main.c` with `kernel/kernel/ustack.c` and
`kernel/kernel/random.c`, and runs the stack builder against a fake machine: a
flat array standing in for physical RAM, a page table that is a list, and four
strict stubs for `pmm_alloc_page`, `pmm_free_page`, `vmm_map_page` and
`copy_to_user_as`/`clear_user_as` that refuse to map a page twice, refuse to map
two virtual pages onto one physical page, and check that the destination is
mapped before every write.

This covers the part of user mode that is pure address arithmetic and therefore
testable anywhere: the order of `argc`/`argv`/`envp`/`auxv`, the `NULL`
terminators, `AT_NULL`, 16-byte `sp` alignment, where the strings are relative to
the tables that point at them, the auxv values, and the refusal paths - an
argument vector too long, a string too big for the stack, and running out of
pages. Getting any of those wrong is not a kernel crash; it is a crash inside a
program that is already in user mode, which is exactly why it gets a test that
does not need a board.

What it does not cover: the real page tables, the MMU, and whether a program
actually finds the stack that way. The in-kernel self test (26 checks) covers the
first two under QEMU, and `init`'s own `auxv` checks cover the third on a real
boot.

### `test_tools.py` - the build tooling

| Class | What it checks |
| --- | --- |
| `Fat16Tests` | the image is a valid MBR + FAT16 volume: `0x55AA`, one bootable partition, BPB fields, volume label `LUMEOS`, directory entries, file contents read back through the FAT table; and that `verify_image.py` accepts a good image and rejects one with a single corrupted data byte |
| `Elf2BinTests` | the ELF-to-flat conversion: physical address requirement, overlap rejection, oversize rejection |
| `CheckIsaTests` | the ISA gate (see §1) |
| `CheckAbiTests` | the EABI gate (see §1), including an integration check against `build/lumeos.elf` when it exists |

### What the host tests cannot prove

Everything about ARM code generation, the MMU, caches, exception entry, interrupt
routing, timers, the UART and scheduling. Those only run on the target. On this
target, "the target" currently means QEMU.

---

## 3. Emulated boot test

```sh
make test-qemu                    # SKIPs with a message if QEMU is absent
python3 tests/qemu/run_qemu_test.py --image build/kernel.img --elf build/lumeos.elf \
        --timeout 60 --required
```

`tests/qemu/run_qemu_test.py` boots the kernel on QEMU's `raspi0` model (a
BCM2835 with an ARM1176, i.e. the Pi Zero W SoC) and watches the serial port for
four boot markers, in order:

```
LumeOS 0.1.0 (armv6kz)                  <- the boot stub reached C code
selftest: running kernel self tests      <- the MMU, pmm, kmalloc, timer, vmm work
LumeOS: boot complete                    <- devices, IRQs and the scheduler are up
main: entering the idle loop             <- the kernel reached its steady state
```

Because the Raspberry Pi firmware is not free software and is not in this
repository, the test boots the raw `kernel.img` (what `start.elf` would hand
over) rather than the SD card image. Three load strategies are tried, because
QEMU's ARM boot path has several meanings for the same option:

| Strategy | QEMU invocation | What it simulates |
| --- | --- | --- |
| `bios` | `-bios kernel.img` | the firmware hand-off: loaded at `0x8000`, entered at `0x8000` |
| `loader` | `-device loader,file=kernel.img,addr=0x8000,cpu-num=0` | an explicit memory write plus a CPU reset into it |
| `kernel` | `-kernel kernel.img` | QEMU's Linux path - a *different* contract: QEMU's own boot stub runs first, with ATAGS at `0x100` |

The third one is included because it is the common way to boot a bare-metal
image in QEMU, but it is not how the Pi firmware boots LumeOS. Without
`--required` (or `$CI`), the test reports SKIP when QEMU is missing, so
`make test-qemu` on a machine without QEMU does not fail the build.

### The guest-error gate

Every strategy runs with `-d guest_errors,unimp -D
build/qemu-guest-errors-<strategy>.log`. Those two of QEMU's log categories
contain nothing but the emulator's opinion of the guest, and this project treats
them differently because they mean different things:

| Log line | What it means | Effect on the run |
| --- | --- | --- |
| `guest_errors` - `bcm2835_ic_write: Bad offset 0`, `Invalid write at addr 0x...`, `pl011_write: Bad offset 0x...` | the kernel touched a register, an offset or an address the hardware does not provide | **fails the run**, by default |
| `unimp` - `bcm2835_property: 0x00010001 get board model NYI` | QEMU has not implemented something the hardware has | reported and counted, never fatal |

The split is not cosmetic. A guest error is the kernel doing something a real
Pi would reject, and it can happen on a boot that looks perfect: nothing else in
this project reads the log the message is written to. An `unimp` line is the
*emulator's* incompleteness - the current one is the property mailbox's "get
board model" tag, which `kernel/arch/arm/memdetect.c` asks for (and only uses to
print a line) and which real firmware answers. Failing the build over that would
mean deleting a working kernel feature to satisfy a model.

Because QEMU writes the message and not the category, the two are told apart by
the wording of the line (`nyi`, `unimplemented`, `not implemented`, `no model`);
anything else counts as a guest error, so if a future QEMU rewords its `unimp`
notes the gate gets noisier, never quieter. `--max-guest-errors N` exists for
the remaining case where a guest error has been examined and accepted: the count
appears in the pass line and in the CI annotation, so a waiver is visible to a
reviewer rather than hidden in a script.

The gate exists because of a real bug. `kernel/arch/arm/irq.c` used to mask all
three interrupt-enable registers at boot and then write the *read-only* pending
register "to clear latched state", which clears nothing. The kernel booted, ran
59 self tests and started a user program with that write in place; on hardware
it is at best a no-op and at worst undefined. It was found by reading QEMU's
interrupt-controller model next to the kernel's register writes, not by any
test - so the test was added in the same commit as the one-line fix.

### What the emulator run currently proves

At the current head the `bios` and `loader` strategies boot; the `kernel`
strategy still fails, as it should, because it uses a different entry contract.
A passing run establishes, from the guest's own serial output:

* all four boot markers print, in order, ending with
  `main: entering the idle loop`;
* the kernel prints `selftest: N/N checks passed` - the physical memory manager,
  the kernel heap, MMU mapping and translation, the system timer, the string
  library, the EABI division helpers, the exception round trip, the ELF loader's
  validation and the initial-stack/auxv layout all behave as their tests
  require. The emulator asserts `N == M` *and* that at least 80 checks ran, so
  the summary line cannot quietly become vacuous. Commit `ed52306` reported
  `59/59` before the 26 stack and auxiliary-vector checks were added; the count
  grows with the kernel, and this document only ever states a number together
  with the commit that showed it;
* the PL011 emits the shell prompt (`lume>`) at the end of a serial log of a few
  kilobytes, so the console, the interrupt path and the receive path are all
  live;
* QEMU reports no guest errors (see above); `unimp` notes about the
  emulator's own gaps are reported but do not fail the run;
* the user program's own checks pass: it prints `init: auxv verified` only after
  finding the auxiliary vector by walking past `argv` and `envp` (so the `NULL`
  terminators must be there), matching `AT_ENTRY` against the address of its own
  `_start`, reading `AT_PHDR` back as a valid ELF header whose `e_entry` matches,
  confirming `AT_HWCAP` has no floating-point bit, and finding 16 non-zero bytes
  at `AT_RANDOM`;
* the exception history QEMU records during the run is quiet (the count below
  is for a boot of the kernel alone; a run that also starts a user program adds
  the syscalls and timer interrupts that program takes): the last exception
  is an ordinary IRQ, there is no prefetch abort, and no `IFAR`/`IFSR` line
  appears at all. Earlier runs ended in a prefetch abort with `IFSR 0x5`
  (section translation fault) at `IFAR 0xffff000c` - a fault inside the
  exception vector page itself.

Two defects were fixed to get here. Both had symptoms that pointed somewhere
other than the cause, which is the only reason they are written down at this
length:

1. **The interrupt storm was a level-held source, not a routing mistake.** The
   emulator counted 1,646,273 exceptions in a ten-second run, nearly all of them
   returning to `uart_rx_ready+0x14` - the PL011 receive interrupt. That reading
   suggests bad interrupt-controller routing, and the hypothesis was attractive
   because the timer and UART line numbering really does differ between the
   BCM2835 manual and QEMU's model. The actual cause was in the driver:
   `uart_irq_handler()` acknowledged only the RX and receive-timeout bits, so
   when the masked interrupt status contained *only* an error condition
   (overrun, break, framing, parity - all of which the PL011 raises and a
   floating or badly wired RX line is enough to cause) it returned without
   acknowledging anything and the line stayed asserted. The handler now clears
   every bit the device reports, with a one-shot warning because such a bit
   usually means the wiring is wrong, and drivers whose line is aggregated into
   the controller's *basic pending* register mark themselves self-checking so
   that `do_irq()` services them even when the two shared pending registers read
   as empty. The same run now reports **74** exceptions instead of 1.6 million.
2. **`vmm_translate()`'s "not mapped" sentinel collided with physical
   address 0.** The kernel maps virtual `0xC0000000` to *physical* 0 - the bottom of
   RAM, where the image is not, but where RAM begins - so a returned 0 could
   mean either "unmapped" or "mapped to physical 0", and the self test asking
   whether the kernel half is visible in a fresh address space could never pass.
   The function now returns `0`/`-1` and writes the physical address through a
   pointer. The test asserts a translation whose physical address is
   distinguishable (`0xC0008000 -> 0x00008000`, the page the image is actually
   loaded into), that a never-mapped address reports failure, and that the
   exception vector page (`0xFFFF0000 -> 0x000F0000`) is present.

None of that is hardware evidence. QEMU implements a BCM2835 *model*: the boot
ROM, `start.elf`, the card, the clock tree, the GPIO configuration and the
board's electrical behaviour are all absent or idealised. What the emulator can
show is that the kernel's own logic is self-consistent; whether the firmware
loads it, whether the UART is wired where the kernel thinks and whether the
timings hold is §5 of [boot-pi.md](boot-pi.md). Until that has happened on a
board, every one of the bullets above carries the label **emulated only**.

The QEMU step is blocking in CI, so a regression that loses a marker, a self
test or the prompt fails the build rather than being reported as a note. The
step deliberately has no `continue-on-error`; when a real bug does appear, the
diagnosis step below supplies the trace.

### Observed versus claimed

Statements about this project are of two kinds, and the difference is the whole
point of this document:

* **observed** - a CI run at a named commit reports it. That is what citations
  like "`59/59` at `ed52306`" mean, and every ✅ in
  [roadmap.md](roadmap.md) is meant to be one of these;
* **pending** - the code and its local gates exist, and the emulator run that
  would confirm it has not been read back yet.

When something is pending, it is written down as pending, with the reason. The
exception round-trip probe spent exactly one commit that way ("written,
`arch/arm/trapprobe.S`; the emulator run that confirms it is pending - pushing
was blocked by a credential problem on the machine that wrote it"), and the next
run turned it into an observed result with the commit that showed it. The
alternative - letting the document quietly promote "the code exists" into "it
works" - is the one failure mode this document set exists to prevent.

Nothing is pending at the current head.

### `tests/qemu/diagnose_boot.py`

When `run_qemu_test.py` fails, CI runs `tests/qemu/diagnose_boot.py`, which
re-runs QEMU with `-d in_asm,int,unimp,guest_errors,cpu_reset` and reports, as
`DIAG:` lines:

* how many translated blocks actually executed, and the first few addresses;
* the same addresses turned into `file:line` through
  `arm-none-eabi-addr2line`, so "where is it stuck" becomes "which C line is it
  stuck in";
* the exception history (which exception, from which mode, with what ESR), the
  faulting `PC` resolved to a symbol, and the `FAR`/`FSR`/`DFAR`/`DFSR` values;
* the number of unimplemented/unassigned accesses QEMU logged (0 in a healthy
  run - it separates "the kernel is wrong" from "the model is incomplete");
* how many bytes came out of the serial port, and the first 120 of them.

The output is published as a GitHub Actions annotation by
`tools/ci_annotate.py`, because the Actions log download host is not reachable
from every environment while the checks API is.

### What the emulated test cannot prove

* **The firmware boot chain.** QEMU never runs `bootcode.bin`/`start.elf`, so
  the `config.txt` settings, the FAT16 layout the firmware reads, the load
  address agreement, the GPU/ARM memory split and the firmware's clock setup are
  all *unverified* until the image is booted on a real board.
* **Peripheral behaviour.** QEMU models the PL011, the system timer, the GPIO
  block and (partially) the mailbox. It does not model the SD card controller,
  the USB stack, the WiFi chip or HDMI, and it models the VideoCore mailbox far
  less strictly than the real firmware does.
* **Timing.** QEMU's virtual clock is not the board's clock. Any bug that
  depends on real interrupt latency, cache behaviour or the firmware being slow
  will not appear.
* **Anything about the physical signal.** Whether the serial adapter is wired
  correctly, whether the card is bootable, whether the board browns out.

A green QEMU run means "the kernel's own logic and the ARM-level bring-up are
sound enough to reach the markers". It is not a substitute for
[hardware.md](hardware.md).

---

## 4. Hardware validation (not done yet)

The procedure to follow on a real Pi Zero W - what to flash, what serial
adapter, what to expect - is in [hardware.md](hardware.md). When hardware
results exist, they belong here with the board revision, the card, the serial
adapter and the exact image SHA-256.

Rules this project follows for hardware claims, because it is easy to
accidentally lie in a README:

1. Do not say "works on the Pi" until the boot markers have been seen on a
   serial console of a real board.
2. Do not describe a behaviour that was observed in QEMU as if it happened on
   hardware.
3. When reporting a hardware result, include the image hash and the board.
4. When something is untested, say so in the same sentence as the feature.

---

## 5. Reading CI results

`.github/workflows/build.yml` publishes:

* an **annotation** for any failed step, containing the tail of that step's log
  (`tools/ci_annotate.py`);
* on failure, a `ci-logs` artifact with the full step logs;
* a `lumeos-sd-image` artifact (the flashable `build/lumeos-sd.img`), whose
  SHA-256 is also written into a `::notice::` annotation;
* a `lumeos-kernel` artifact with `build/lumeos.elf` and `build/kernel.img`
  (the ELF carries symbols for `addr2line`/`gdb`);
* the QEMU boot diagnosis annotation when the QEMU step fails, plus a
  `notice` annotation with the passing run's markers and self test counts (so
  "it booted" is checkable after the fact, not only on failure).

Useful queries with the GitHub CLI:

```sh
gh run list --limit 5
gh run view <run-id>                       # step list and conclusions
gh api repos/kiddoz-lab/lumeOS/commits/<sha>/check-runs     # annotations live here
```

---

## 6. Adding a test

* **Portable logic** (no hardware, no arch-specific code): add it to the kernel
  sources *and* to the host test. The division cores are the model: the
  arithmetic lives in `kernel/kernel/divmod.c`, is compiled for the host by
  `test_kernel_lib.py`, and is exercised on the target by the self tests.
* **Kernel invariants** that need the target (page allocator, MMU, timer,
  scheduler, trap frames): add a check to `kernel/kernel/selftest.c`. It runs on
  every boot and the result is part of the boot log, so it works on QEMU and on
  a real board with the same code.
* **Boot behaviour**: add a marker to the list in `tests/qemu/run_qemu_test.py`.
  Markers must be strings the kernel really prints - never invent a marker to
  make a test pass.
* **Build tooling**: add to `tests/host/test_tools.py`, including a negative
  case (a corrupted input that must be rejected).
