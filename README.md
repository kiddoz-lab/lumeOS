# LumeOS

An operating system for the Raspberry Pi Zero W, written from scratch.

LumeOS is a small 32-bit ARMv6 kernel for the BCM2835 SoC (ARM1176JZF-S core,
512 MiB RAM). It brings up the MMU, talks to the serial console, takes
interrupts from the BCM2835 interrupt controller, runs a preemptive scheduler
and drops into its own kernel shell. It is built from public documentation:
there is no Linux kernel underneath, none of the code is copied from Linux, and
the only external code that ever runs is the Raspberry Pi boot firmware the
board needs to start at all.

The long-term goal is a system that can run ordinary 32-bit ARM Linux ELF
executables unmodified - which means implementing the syscall surface,
memory semantics, processes, signals and filesystems those binaries expect, not
just an ELF parser. See [docs/userspace.md](docs/userspace.md) and
[docs/roadmap.md](docs/roadmap.md) for how that is being approached.

## Status

**Bring-up, and honest about it.** The kernel compiles, links and converts into
the exact flat image the Raspberry Pi firmware loads at physical `0x8000`, and
its logic is covered by host unit tests and an instruction-set gate. It has
**never been run on real hardware** - I do not have a Pi Zero W in this
environment - so every claim below about booting is a claim about code that
exists and is checked to the extent an emulator and a host machine allow.

| Area | State |
| --- | --- |
| Build system, linker script, ISA gate | works, verified locally and in CI |
| Flat `kernel.img` at load address `0x8000` | works, verified in CI |
| Bootable SD image (MBR + FAT16 + firmware) | built by CI as an artifact |
| Host unit tests (`printf`, string library, division cores, build tools, both gates) | 23 tests pass locally and in CI |
| Static gates (ARMv6 ISA, EABI calling convention) | pass on every build; enforced in CI |
| In-kernel self tests | `59/59` under QEMU (`ed52306`); printed on every boot and asserted by CI (`N == M`, at least 55) |
| Boot stub, MMU, caches, high vectors | 🟡 executes under QEMU and reaches all four boot markers; **unverified on hardware** |
| Exceptions, IRQs, system timer, scheduler | 🟡 ticks, dispatches IRQs and idles in QEMU without faulting (**unverified on hardware**) |
| Serial console (PL011, 115200 8N1) | 🟡 prints, and the shell prompt reaches the terminal, under QEMU; **unverified on hardware** |
| Kernel shell | 🟡 prompt appears under QEMU; no command has been typed into it yet |
| Framebuffer / input / storage / networking | **not implemented** |
| ELF loader (static ARM `ET_EXEC`) | ✅ loads, maps and enters a program; refuses PIE and foreign architectures by name |
| Userspace: syscalls, processes, consoles | 🟡 a static ARM ELF runs in user mode, makes syscalls (`write`/`read`/`brk`/`uname`/`wait4`/…), is given `argv`/`envp`/`auxv` in the Linux layout and exits with a status the kernel reaps; **unverified on hardware** |
| VFS, filesystems, signals, pipes, `fork`, `futex`, `mmap` | **not implemented** (they return `-ENOSYS` and are logged once each) |
| A real C library (`glibc`/`musl`) | **does not start yet**: `auxv` is in place, but a libc wants `mmap2`, `mprotect`, `clock_gettime` and signals, and no filesystem exists to load it from |

The distinction matters and is maintained everywhere in this repository:
*done* means it builds and is covered by a test that actually ran, *unverified*
means the code exists but nothing has executed it, and *not implemented* means
exactly that.

## Quick start

```sh
# 1. Build the kernel image (needs an ARM cross compiler or zig, see below)
make kernel
#    -> build/lumeos.elf, build/kernel.img

# 2. Run the tests that do not need a board
make test-host
python3 tools/check_isa.py --require-attributes --require-capstone build/lumeos.elf

# 3. Build a flashable SD card image (needs the Raspberry Pi boot firmware)
make firmware                    # downloads bootcode.bin, start.elf, fixup.dat
make image                       # -> build/lumeos-sd.img (structure verified)

# 4. Optional: boot it under QEMU (needs qemu-system-arm; currently expected to
#    fail part-way - see docs/testing.md - so it is not part of the default build)
make test-qemu
```

Full details, including the no-root toolchain fallback, are in
[docs/building.md](docs/building.md).

## What you get

* `build/lumeos.elf` - the linked kernel with symbols (for `gdb`, `nm`, `objdump`)
* `build/kernel.img` - flat binary for the Raspberry Pi firmware (`0x8000`)
* `build/lumeos-sd.img` - MBR + FAT16 image containing the firmware,
  `config.txt` and `kernel.img`: write it to a microSD card and the board has
  everything it needs to try to boot LumeOS

CI builds all three and publishes the image as an artifact, so you can flash a
build without a local toolchain.

## Repository layout

```
kernel/
  ld/lumeos.ld        linker script: LMA 0x8000, VMA 0xC0008000, explicit PHDRS
  arch/arm/           ARM1176/BCM2835 specific: boot.S, vectors.S, MMU, IRQ,
                      exceptions, system timer, memory detection, startup
  drivers/            PL011 UART, GPIO, VideoCore mailbox, serial input
  mm/                 physical page allocator, kernel heap, user access helpers
  kernel/             portable kernel: scheduler, threads, processes, fds,
                      timekeeping, klog, panic, printf, string library,
                      self tests, kernel shell
  include/lume/       kernel headers (hw/bcm2835.h holds the register map,
                      divmod.h the portable division cores behind the EABI
                      helpers)
tools/                build tools: ELF->flat binary, image builder and verifier,
                      ISA checker, EABI checker, firmware fetcher, CI annotation
                      helper, zig cc wrapper
tests/
  host/               unit tests that run on the development machine
  qemu/               emulator boot test (QEMU raspi0 model) and the boot
                      diagnosis tool
config.txt            Raspberry Pi boot configuration copied into the image
docs/                 architecture, building, testing, hardware, boot chain,
                      userspace/ABI plan, roadmap, research notes
```

The build artefacts are `build/lumeos.elf` (with symbols),
`build/kernel.img` (the flat image the firmware loads) and
`build/lumeos-sd.img` (the flashable card image). Nothing generated is committed.

## Documentation

| Document | Contents |
| --- | --- |
| [docs/architecture.md](docs/architecture.md) | how the kernel is put together: boot, memory map, MMU, exceptions, scheduler, the two ARM calling conventions, coding rules |
| [docs/building.md](docs/building.md) | toolchains (including a no-root fallback), build targets, troubleshooting |
| [docs/testing.md](docs/testing.md) | the four levels of testing, what each one does *not* prove, what the emulator run currently establishes, and how to read CI results |
| [docs/hardware.md](docs/hardware.md) | Raspberry Pi Zero W specifics, flashing, serial console, first boot |
| [docs/boot-pi.md](docs/boot-pi.md) | the Raspberry Pi boot chain and the assumptions LumeOS makes about it |
| [docs/userspace.md](docs/userspace.md) | the ARM Linux ABI plan: syscall convention and numbers, structure layouts, ELF loading, TLS, how the compatibility layer is split from the native one |
| [docs/roadmap.md](docs/roadmap.md) | milestones from here to running real ARM Linux binaries, including the known bugs at the current head |
| [docs/research-notes.md](docs/research-notes.md) | every public reference the implementation is based on, what was taken from each, and what is still uncertain |
| [THIRD_PARTY.md](THIRD_PARTY.md) | licences of everything LumeOS depends on or downloads |

## Requirements

* an ARM cross toolchain: `arm-none-eabi-gcc` (Debian/Ubuntu package
  `gcc-arm-none-eabi`) **or** [zig](https://ziglang.org) as a drop-in
  replacement (`make CC=tools/zig-cc.sh`)
* Python 3.9+ for the build tools, with
  [capstone](https://www.capstone-engine.org/) for the instruction-set gate
* the Raspberry Pi boot firmware for the SD image: downloaded by
  `tools/fetch-firmware.sh` at a pinned commit, or copied from any Raspberry Pi
  OS card. It is not redistributed here (see [THIRD_PARTY.md](THIRD_PARTY.md))

## Licence

LumeOS is MIT licensed - see [LICENSE](LICENSE). Third-party licences, including
the ones that apply to the Raspberry Pi boot firmware and to the Linux ABI
documentation the syscall layer is modelled on, are listed in
[THIRD_PARTY.md](THIRD_PARTY.md).
