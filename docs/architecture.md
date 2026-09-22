# LumeOS architecture

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
* Every build runs `tools/check_isa.py`: it checks the ARM build attributes and
  disassembles every executable section with capstone to reject instructions the
  ARM1176JZF-S cannot execute. CI passes `--require-capstone` so the gate cannot
  silently degrade into a no-op.
* No floating point anywhere in the kernel.
* Hardware register access goes through `kernel/include/lume/hw/bcm2835.h`
  constants and one accessor macro per driver; no magic numbers in C files.

## Known weaknesses

These are real and tracked, not hidden:

* **The trap frame save order in `vectors.S` is wrong for r8/r9.** The IRQ and
  abort entries save r8/r9 before the frame is complete; on a real core this can
  corrupt a register on return. It is the first thing to fix when the kernel
  first runs.
* **`__restore_regs` restores the user bank unconditionally**, so returning to a
  kernel thread through that path would do the wrong thing.
* No cache maintenance strategy beyond explicit clean/invalidate around mailbox
  messages; DMA-capable drivers will need a coherent-mapping helper.
* The MMU maps only the first 256 MiB of RAM at boot; boards with more usable
  RAM (none in our target, but the Pi 2/3 in QEMU) get the rest unmapped.
* No locking: interrupts are disabled around critical sections instead, which is
  acceptable on a single core and will stay that way until SMP is a goal.
