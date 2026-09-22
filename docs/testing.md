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

### Where the boot currently stops

As of the current branch, the `bios` strategy gets a long way and then fails:

* the kernel reaches C code, initializes the PL011 and *prints* - so the boot
  stub, the MMU, the peripheral mapping and the baud-rate setup all work (the
  diagnosis reports how many bytes came out and the first 120 of them);
* it initializes the system timer, the interrupt controller and the scheduler,
  and execution reaches the scheduler's context switch, the wait queues and the
  serial input driver;
* then the interrupt path takes over: the emulator counts over a million IRQs in
  a ten-second run (mostly at `uart_rx_ready+0x14`, i.e. the PL011 RX
  interrupt), and the last exception QEMU reports is a prefetch abort with
  `IFSR 0x5` (section translation fault) at `IFAR 0xffff000c` - inside the
  exception vector page itself.

The lesson from that trace is recorded in
[architecture.md](architecture.md): the boot stub maps the vector page at
`0xFFFF0000`, and the kernel runs with the high-vector configuration, so a
fault *in the vector entry* means the dispatch of the exception failed, not
that the exception occurred.

The step is marked `continue-on-error: true` in CI and this is deliberate: the
job records a full diagnosis instead of turning the build red while a real bug
is being worked on. The current suspicion list (from the emulator's own trace,
not from guesswork) is in [roadmap.md](roadmap.md#known-bugs-at-the-current-head).

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
* the QEMU boot diagnosis annotation when the (informational) QEMU step fails.

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
