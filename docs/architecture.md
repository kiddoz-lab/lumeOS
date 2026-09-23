# LumeOS architecture

This is the design document: how the kernel is put together and why. The other
documents go deeper on specific areas:

* [building.md](building.md) - toolchains, targets, the static gates, troubleshooting
* [testing.md](testing.md) - what is tested and what each test does not prove
* [hardware.md](hardware.md) - the board, flashing, the serial console
* [boot-pi.md](boot-pi.md) - the firmware boot chain and the assumptions LumeOS makes
* [userspace.md](userspace.md) - the ARM Linux ABI plan
* [roadmap.md](roadmap.md) - milestones and the known bugs at the current head
* [research-notes.md](research-notes.md) - the public references behind the implementation

This document describes how the kernel is put together today. Everything
labelled *unverified* is code that exists and compiles but has never executed
on a CPU - see [testing.md](testing.md) for the evidence status of each layer.

## Design goals

1. **Boot a Raspberry Pi Zero W and stay up.** No dependencies, no firmware
   support beyond what the board requires to start.
2. **Keep the ARM1176JZF-S happy.** ARMv6KZ only: no ARMv7 instructions, no VFP
   (the core in the Zero W has no floating-point unit), no unaligned accesses.
3. **Separate hardware from policy.** Only `kernel/arch/arm/` and
   `kernel/drivers/` know that a BCM2835 exists. Everything in `kernel/kernel/`
   and `kernel/mm/` is portable C and would compile for another 32-bit ARM SoC
   with a different `arch/` directory.
4. **Grow towards running unmodified ARM Linux binaries.** The ABI work is
   described in [userspace.md](userspace.md); the kernel is being shaped so that
   syscalls, processes, signals and filesystems can be added without rewrites.
5. **Never fake it.** A stub that returns success without doing the work is
   worse than a missing feature, because it hides the work that remains.

## Boot path (unverified on hardware)

```
VideoCore GPU ROM
  -> bootcode.bin     (loads SDRAM, then start.elf)
    -> start.elf      (reads config.txt, loads kernel.img to 0x8000, jumps to it)
      -> LumeOS _start at physical 0x8000, MMU off, caches off
```

`kernel/arch/arm/boot.S` then:

1. switches to SVC mode with IRQs and FIQs masked (the firmware may leave the
   core in Monitor mode on a TrustZone-capable boot path, so the mode is set
   explicitly rather than assumed);
2. invalidates I-cache, D-cache, branch predictor and TLB;
3. copies the 64-byte exception vector template to physical `0xF0000`;
4. zeroes the level 1 translation table at physical `0x4000` and fills it;
5. enables the MMU with high vectors (`SCTLR.V=1`) and strict alignment
   (`SCTLR.A=1`), then branches into the kernel's virtual alias;
6. calls `kernel_main(fdt_pa, load_pa)` in C.

The stub is position independent and uses no stack, because it runs before
there is one.

## Memory map

| Physical | Virtual | Use |
| --- | --- | --- |
| `0x00004000` | (identity, temporary) | level 1 translation table (16 KiB) |
| `0x00008000` | `0xC0008000` | kernel image (`.text`, `.rodata`, `.data`, `.bss`) |
| `0x000F0000` | `0xFFFF0000` | exception vector page (`VBAR`-style high vectors) |
| `0x20000000` | `0xF0000000` | BCM2835 peripherals (32 MiB, device memory) |
| `0x20003000` | `0xF0003000` | system timer (CLO/CHI/C3) |
| `0x20200000` | `0xF0200000` | GPIO |
| `0x20201000` | `0xF0201000` | PL011 UART0 (serial console) |
| `0x2020B880` | `0xF020B880` | VideoCore mailbox |
| `0x2000B000` | `0xF000B000` | interrupt controller (IRQ pending/basic/enable) |
| RAM | `0xC0000000 + pa` | 256 MiB of RAM mapped at boot (sections) |
| RAM | `0xE0000000 + pa` | uncached alias, 32 MiB, for DMA-style buffers |

The mapping is a single level 1 table of 1 MiB sections, built in assembly
before the MMU is on. User processes will get their own table with the same
kernel regions but per-process user sections (see
[kernel/arch/arm/mmu.c](../kernel/arch/arm/mmu.c)).

The kernel virtual base is `0xC0000000` because that is where an ARM Linux
binary expects to find itself once the userspace ABI work begins; keeping the
kernel on the same side of the address space avoids re-mapping later.

## Exceptions and interrupts

`kernel/arch/arm/vectors.S` holds the vector table. Each entry builds a trap
frame (`struct trapframe`, 0x48 bytes: r0-r12, sp, lr, pc, cpsr) on the current
stack and calls into C:

| Exception | C entry | Notes |
| --- | --- | --- |
| SVC | `exception_svc` -> `do_syscall` | syscall number in `r7`; the current implementation logs and either exits the process (user mode) or panics (kernel mode) |
| IRQ | `exception_irq` -> `irq_dispatch` | reads `IRQ_PENDING_1/2` and `IRQ_BASIC_PENDING` |
| data/prefetch abort | `exception_data_abort`, `exception_prefetch_abort` | logs address/status, kills the process |
| undefined instruction | `exception_undefined` | logs, kills the process |

`irq_dispatch()` (`kernel/arch/arm/irq.c`) walks the two shared pending
registers and calls the handler registered for each of the 64 shared interrupt
lines. The system timer (IRQ 1, compare channel 3), the USB controller (IRQ 9),
the PL011 UART (IRQ 57) and the SD/EMMC controller (IRQ 62) are the lines the
BCM2835 documentation defines that LumeOS currently cares about.

The system timer driver (`arch/arm/timer.c`) reads the 1 MHz free-running clock
(`CLO`/`CHI`, with a double read to avoid a torn 64-bit value), programs compare
channel 3 and calls `sched_tick()`. It also survives a misrouted interrupt by
polling the compare register a bounded number of times from the idle loop - a
deliberate belt-and-braces choice while the routing is unverified on hardware.

## Scheduling, threads and processes

* `kernel/kernel/sched.c` - a simple round-robin scheduler over a static table
  of threads with a 100 Hz tick. `schedule()` picks the next runnable thread and
  `__switch_to()` (in `arch/arm/context.c`) switches `r4-r11`, `sp` and the
  program counter.
* `kernel/kernel/thread.c` - kernel threads. A kernel thread starts at
  `__kernel_thread_entry` with the entry function in `r4` and its argument in
  `r5`; when it returns, `kthread_exit()` cleans up.
* `kernel/kernel/proc.c` - processes. A process owns a page table, an address
  space description, a file descriptor table and at least one thread. User
  threads have a trap frame that the exception return path uses to enter ARM
  user mode; `kernel/arch/arm/exception.c` implements `__restore_regs` for both
  kernel and user returns.
* `kernel/kernel/fd.c` - a refcounted file descriptor pool. There is no VFS
  yet, so nothing is registered in it at boot.

## Memory management

* `kernel/mm/pmm.c` - physical memory: a 16 KiB bitmap over 4 KiB pages with
  first-fit allocation, plus reserve calls for regions that must never be handed
  out (firmware low memory, the kernel image, the vector page, the device tree).
* `kernel/mm/kmalloc.c` - kernel heap (256 KiB initial) with coalescing free
  blocks, grown from the page allocator.
* `kernel/mm/uaccess.c` - `copy_to_user`/`copy_from_user` and friends, which
  validate user addresses and will handle the fault-recovery path once
  userspace exists.
* `kernel/arch/arm/memdetect.c` - asks the firmware what RAM the ARM may use
  (mailbox tag `GET_ARM_MEMORY`), falling back to a conservative compile-time
  default for a 512 MiB board with the default 64 MiB GPU split. The log always
  says which source was used.

## Devices and logging

`kernel/kernel/klog.c` is a 16 KiB ring buffer with up to four consoles, so
early boot messages survive long enough to be read even if the console appears
late. `kernel/drivers/` contains the PL011 UART (system console), GPIO, the
VideoCore mailbox property interface (`GET_ARM_MEMORY`, `GET_CLOCK_RATE`,
board model, framebuffer tags), and the serial input path that feeds the kernel
shell. `kernel/kernel/kshell.c` is a bring-up tool on the serial port
(`help`, `mem`, `ps`, `time`, `irq`, `echo`, `reboot`, `halt`, `version`) - it is
not the userspace shell.

## Coding rules

* C11, freestanding: no libc, no compiler runtime beyond the AEABI helpers
  implemented in `kernel/kernel/aeabi.c` (`__aeabi_uidiv`, `memcpy` family...).
* `-ffreestanding -fno-builtin -fno-stack-protector -mno-unaligned-access`,
  `-mfloat-abi=soft`, `-march=armv6kz -mcpu=arm1176jzf-s`.
* Every build runs two static gates on the linked ELF:
  * `tools/check_isa.py` checks the ARM build attributes and disassembles every
    executable section with capstone to reject instructions the ARM1176JZF-S
    cannot execute. It steps over literal pools (`skipdata`); capstone stops at
    the first undecodable word, which once limited the scan to 235 of 10231
    instructions and turned the gate into a prefix check.
  * `tools/check_abi.py` checks the EABI division helpers against the fixed
    rtabi layout (see below).
  CI passes `--require-capstone` to both so neither gate can silently degrade
  into a no-op.
* No floating point anywhere in the kernel.
* Hardware register access goes through `kernel/include/lume/hw/bcm2835.h`
  constants and one accessor macro per driver; no magic numbers in C files.
  Peripherals are *not* in the RAM alias window: `PHYS_TO_VIRT()` (0xC0000000 +
  physical) is for RAM only, and register addresses must go through
  `PERIPHERAL_TO_VIRT()` (0xF0000000 + offset from 0x20000000), because that is
  where `boot.S` maps them. Using the RAM alias compiles fine and faults at
  runtime before any console exists.

### The two ARM calling conventions, and why the EABI helpers are assembly

The compiler emits calls to `__aeabi_uidiv`, `__aeabi_uldivmod` and friends
with the register layout fixed by the public *Run-time ABI for the ARM
Architecture* addendum: operands in r0-r3 and, for the multi-word helpers,
quotient in r0:r1 and remainder in r2:r3.

That is **not** what this toolchain does for an equivalent C function. LLVM
returns every composite type in memory through a hidden pointer in r0 ("sret"),
even a two-word struct. So a helper written as

```c
struct u64_divmod __aeabi_uldivmod(u64 num, u64 den);   /* WRONG */
```

gets a body that reads its divisor from a stack slot nobody wrote and returns
its result into `*(u64 *)num`, while its callers pass the operands in r0-r3 and
read the results back from r0-r3. Nothing warns; the linker is happy; the kernel
panics with "64-bit division by zero" while the divisor it was handed was 1000.

Consequences, both enforced by tools:

* the multi-register helpers live in `kernel/arch/arm/aeabi_div.S` and only
  marshal registers, calling the portable cores in `kernel/kernel/divmod.c`;
* `tools/check_abi.py` disassembles the linked kernel and fails the build if an
  EABI helper stores its result through r0, or if a call site dereferences r0
  straight after a multi-word call.

The same rule applies to any future interface whose ABI is defined outside this
tree - most importantly the syscall entry point: the ARM Linux syscall ABI uses
registers and caller-provided pointers, never register-returned structs, which
is fortunately the same shape.

## System calls and user mode

The path a program takes into the kernel, and the reason each step is where it
is:

```
 user program: mov r7, #4 ; svc #0
        |
        v
 vector_svc (vectors.S)      builds the trap frame on the SVC stack *in place*
        |                    (see the SVC leak note below), records the mode it
        |                    trapped from, masks IRQs/FIQs, calls the handler
        v
 do_syscall (exception.c)    recognises the self-test probe's SVC, then calls
        |                    syscall_dispatch() with the frame
        v
 syscall_dispatch (syscall.c) number in r7, arguments r0-r5, result in r0;
        |                    unimplemented numbers return -ENOSYS and are logged
        |                    once each
        v
 kernel services             console device via the fd table, uaccess for user
                             pointers, vmm/pmm for brk, processes for exit
        |
        v
 __restore_regs (vectors.S)  writes SPSR and returns with "movs pc, lr", which
                             switches to user mode and branches in one
                             instruction, so an interrupt cannot arrive with the
                             frame half popped
```

Three properties of this path are load-bearing and are the things to check first
if userspace misbehaves:

* **the frame's meaning depends on where the trap came from.** A trap from user
  mode carries the user bank's `sp`/`lr`; a trap from a privileged mode carries
  the interrupted *kernel* stack pointer and the resume address. The entry code
  is what makes that true, and `lume/trapframe.h` states the contract.
* **the kernel always runs with interrupts masked except at its explicit wait
  points.** The entry masks them in the CPSR it runs on without disturbing the
  status word the return restores, so a trap from user mode (where IRQs are on)
  never means a handler starts with them on.
* **user pointers are validated before they are used, in the address space that
  will be used.** `copy_to_user()`/`copy_from_user()` check the range against the
  live page tables; the `_as()` forms install the address space they were given
  for the duration of the copy, which is what the boot-time stack build needs
  (`uaccess` and the MMU must agree about which space is in play, or the copy
  validates in one space and faults in another).

## The ELF loader

`elf_load()` maps a static `ET_EXEC` image into a fresh address space; the
implementation notes - what is refused by name, why pages shared by two segments
must be mapped once, and why the I-cache must be invalidated before the first
user instruction - are in [userspace.md](userspace.md#4-elf-loading). The
loader is deliberately strict: it would rather refuse an image with a named
reason than map half of it and let the program fail somewhere less informative.

## Known weaknesses

These are real and tracked, not hidden:

* **The r8/r9 save order is measured now, not argued about.** An earlier version
  of the entry code saved r8/r9 before the frame was complete. The probe in
  `arch/arm/trapprobe.S` puts a known pattern in all thirteen registers, takes a
  real SVC, and fails a named check if any register - r8 and r9 included - does
  not come back unchanged; it also checks that the kernel stack pointer is
  exactly where it was, a check that failed on its first run and found a
  64-byte-per-syscall stack leak in the SVC vector. What the probe still cannot
  reach is the *user-bank* half of `__restore_regs`: it runs in SVC mode, so the
  path a user thread takes is only exercised when `init` actually runs (which it
  does - see [userspace.md](userspace.md) - but not by a self test).
* **`__restore_regs` restores the user bank unconditionally**, so returning to a
  kernel thread through that path would do the wrong thing.
* No cache maintenance strategy beyond explicit clean/invalidate around mailbox
  messages; DMA-capable drivers will need a coherent-mapping helper.
* The MMU maps only the first 256 MiB of RAM at boot; boards with more usable
  RAM (none in our target, but the Pi 2/3 in QEMU) get the rest unmapped.
* No locking: interrupts are disabled around critical sections instead, which is
  acceptable on a single core and will stay that way until SMP is a goal.
