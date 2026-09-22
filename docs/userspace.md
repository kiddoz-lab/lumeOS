# Userspace and the ARM Linux ABI

The long-term goal of LumeOS is to run ordinary 32-bit ARM Linux ELF
executables **unmodified**, and eventually to build and run a real `bash`. That
is a compatibility target, not a feature: it means implementing the syscall
surface, the memory semantics, the process model, the signal behaviour and the
filesystem semantics those binaries depend on. An ELF parser alone gets you a
program that faults on its first `write()`.

This document describes the compatibility plan, the ABI facts the
implementation is being built against, and - just as importantly - what exists
today. **Today, userspace does not exist**: there is no ELF loader, no syscall
layer, and `kernel_main()` prints that the hand-off is not implemented instead
of pretending (see [roadmap.md](roadmap.md) for the order of work).

---

## 1. What "runs ARM Linux binaries" actually requires

A statically linked ARM Linux program built with glibc or musl needs, at
minimum:

| Requirement | Why a program dies without it |
| --- | --- |
| ELF loading with correct alignment and permissions | `PT_LOAD` segments must land at their `p_vaddr`, with the right `p_flags`, and `.bss` zeroed |
| A stack with the Linux `argc/argv/envp/auxv` layout | the C runtime reads `argc` from `sp` and walks `argv`/`envp`; `auxv` gives `AT_PAGESZ`, `AT_HWCAP`, `AT_RANDOM`, `AT_ENTRY` |
| `AT_HWCAP` matching the CPU | glibc/musl choose memcpy/memcmp implementations and (on ARMv7) the VFP path from it. Wrong HWCAP means an illegal instruction at startup |
| The ARM EABI syscall convention, with real numbers | `swi #0` with the number in `r7` and arguments in `r0-r6` |
| `brk`/`mmap`/`mprotect`/`munmap` with real page semantics | `malloc` grows the heap with `brk`, or uses `mmap` for large blocks |
| File descriptors, `openat`, `read`, `write`, `stat64`, `fstat64`, `getdents64` | the dynamic loader reads files and directories; libc initializes stdio |
| Signals with correct `sigaction`/`rt_sigreturn` semantics | the runtime installs handlers, and `rt_sigreturn` must restore the exact interrupted context |
| `clone`-based threads (`CLONE_VM`, `CLONE_SETTLS`) | TLS for glibc/musl is set through `__ARM_NR_set_tls` or `CLONE_SETTLS`; threads use `futex` |
| `futex` | every lock in a modern libc is a futex operation |
| `/proc/self/maps`, `/proc/self/auxv`, `/dev/urandom`, `/dev/null`, `/dev/tty` | the loader, sanitizers, `getrandom`, and anything that prints to a terminal |
| `clock_gettime` with a monotonic clock | timing in the runtime and in every program that sleeps |

That list is the shape of the work, not a wish list: each row is a subsystem
that has to exist and be tested, and each one is a milestone in
[roadmap.md](roadmap.md).

### What "not faking it" means here

The project rules are explicit about this, so they are repeated:

* no custom program named `bash` masquerading as Bash - the target is the real
  binary, and progress is measured by how much of the real binary's requirements
  are implemented;
* no syscall that returns success without doing the work (a `write()` that
  discards data is worse than an unimplemented one, because the next layer
  cannot tell);
* unimplemented paths must fail loudly and specifically - the current
  `do_syscall()` prints the syscall number and the caller's PC, then terminates
  a user-mode caller with `SIGSYS` (128 + 31) and panics a kernel-mode one;
* syscall numbers and structure layouts come from the published ABI headers, and
  are recorded in [research-notes.md](research-notes.md) with their sources.

---

## 2. The ABI, concretely

The numbers and layouts below come from the ARM Linux UAPI headers
(`arch/arm/include/uapi/asm/unistd.h`, `asm/stat.h`, `asm/signal.h`,
`asm-generic/*`). They are the contract; they are not negotiable, and they are
not allowed to be guessed in code.

### Syscall convention

| | |
| --- | --- |
| Instruction | `svc #0` (historically `swi #0`) |
| Number | `r7` |
| Arguments | `r0`, `r1`, `r2`, `r3`, `r4`, `r5` (a seventh goes on the stack) |
| Return | `r0`; negative values in `[-4095, -1]` are `-errno` |
| Restart | the kernel rewrites `r0` with `__NR_restart_syscall` (0) or returns `-ERESTARTSYS` so libc can restart |
| ARM-private calls | `__ARM_NR_BASE` = `0x0f0000`; `__ARM_NR_set_tls` = `0x0f0005` |
| Legacy calls | `0x900000 + old_nr` (the original ARM private space), e.g. `0x900000 + 120` = `clone` |

### Structures that must be laid out exactly

| Structure | Layout |
| --- | --- |
| `struct stat64` (`fstatat64`) | 64 bytes on 32-bit ARM; `st_mode` at offset 16, `st_size` at offset 44, `st_blocks` at 60 (EABI layout, not the x86 one) |
| `struct sigaction` | `{ handler, sigset_t (8 bytes), flags, restorer }` = 16 bytes for a 32-bit sigset |
| `struct utsname` | six fields of 65 bytes: `sysname`, `nodename`, `release`, `version`, `machine`, `domainname` |
| `struct old_utsname` | five fields (no `domainname`) - some programs still ask for it |
| `struct timespec` / `timeval` | two words each (`tv_sec`, `tv_nsec`/`tv_usec`), `time_t` is 32-bit on ARM |
| `struct iovec` | `{ void *base; size_t len; }` |
| `struct pollfd` | `{ int fd; short events; short revents; }` (8 bytes) |
| `struct dirent64` | `{ ino64, off64, d_reclen, d_type, d_name... }` |

### The syscalls that matter first

In rough dependency order - this is the order the milestone list follows:

```
exit 1            write 4          read 3           open 5           close 6
lseek 19          getpid 20        brk 45           ioctl 54         fcntl 55
uname 122         mmap2 192        munmap 91        mprotect 125     stat64 195
fstat64 197       getdents64 217   openat 322       fstatat64 327    clock_gettime 263
nanosleep 162     rt_sigaction 174 rt_sigprocmask 175 rt_sigreturn 173
clone 120         wait4 114        futex 240        set_tls 0x0f0005 pipe2 359
getrandom 384     exit_group 248
```

(`mmap2` takes a page offset rather than a byte offset - a classic source of
"works for small allocations, fails for large ones" bugs.)

### Thread-local storage

ARM Linux has three TLS variants; the kernel only has to support the *user*
access methods a libc actually asks for. glibc on ARM EABI uses the
`kuser_helper` (kernel-provided user-space helper pages at a fixed address) or
`CLONE_SETTLS`; musl uses `set_tls`/`CLONE_SETTLS`. Supporting `__ARM_NR_set_tls`
plus `CLONE_SETTLS` covers both. The kernel's job is to store a per-thread value
and put it in the right register (`cp15 c13` thread id under the MMU) on the
next context switch.

### HWCAP

`AT_HWCAP` must describe this CPU honestly: ARMv6KZ with no VFP, no NEON, no
Thumb-2, no TLS register usable by the kernel's scheme, and the ARMv6
byte/halfword/`swp` instruction set. A libc that sees `HWCAP_VFP` will happily
execute VFP instructions and die with an undefined instruction, which is why
`do_undef()` prints the "this looks like a coprocessor/VFP instruction" hint
when it sees a coprocessor encoding.

---

## 3. How the compatibility layer is structured

The intention is one kernel, two syscall personalities, sharing everything
below the entry point:

```
 userspace binary
        |  svc #0, nr in r7
        v
 arch/arm/vectors.S          -> the SVC vector, saves a struct trapframe
        |
        v
 arch/arm/exception.c        -> do_syscall(): validate, then dispatch by number
        |
        +--> kernel/syscalls/linux.c   -> the ARM Linux ABI (numbers, structs, errno)
        +--> kernel/syscalls/native.c  -> LumeOS's own calls (klog, framebuffer, ...)
                    |
                    v
             portable kernel services: vfs, memory, processes, signals, scheduler
```

Rules that keep this maintainable:

* **the ABI layer is thin** - it validates user pointers, translates structures,
  and calls the same internal function the native call uses;
* **no ARM Linux physics leaks into the scheduler or the VFS**: `mmap2` becomes
  `vmm_map_user_range()`, `clone` becomes `thread_create_user()`, and the
  differences (flags, TLS, `CLONE_VM`) are handled in the ABI file;
* **the numbers live in one header** (`kernel/include/lume/abi/linux_arm.h`
  when it exists), transcribed from the UAPI headers with the source named in a
  comment next to each block. Inventing a number is a bug even if the test
  passes.

### The two personalities

| | Linux ABI | LumeOS native |
| --- | --- | --- |
| Number space | `0..~450` from the UAPI headers | `0x4C000000 + n` (a range Linux will never use) |
| Purpose | run unmodified ARM Linux binaries | give LumeOS programs access to things Linux does not have (kernel log ring, framebuffer control, board info), and give tests a stable, documented surface |
| Stability | fixed by Linux forever | LumeOS's own, versioned, may change |

Both end up in the same `do_syscall()` switch; a native call number is
distinguishable by the high bits alone, so user code cannot accidentally hit a
Linux call through the native space.

---

## 4. ELF loading

A 32-bit ARM Linux ELF is `EM_ARM`, little-endian, `ELFCLASS32`, with
`EABI version 5` in the flags (`EF_ARM_EABI_VER5`), and glibc/musl binaries have
one `PT_INTERP` (the dynamic loader) or none (static). The loader's job:

1. validate the header: class, machine, endianness, EABI version, type
   (`ET_EXEC` first; `ET_DYN`/PIE needs a load bias, which is a later step);
2. for each `PT_LOAD`: allocate pages, copy `p_filesz` bytes, zero the tail of
   the last page and any `.bss`, and apply permissions (`R`, `RX`, `R`+`W`) with
   `W^X` respected. The ARM1176 has separate I/D caches, so instruction pages
   must be cleaned before execution (a cache-coherence step that is easy to
   forget and produces "the right bytes, the wrong instruction" bugs);
3. build the initial stack: `argc`, `argv[]`, `NULL`, `envp[]`, `NULL`,
   `auxv[]` (`AT_PAGESZ`, `AT_HWCAP`, `AT_ENTRY`, `AT_PHDR`, `AT_RANDOM`, ...),
   with `sp` 8-byte aligned as the EABI requires;
4. map the program's `brk` base right after its last segment
   (`LUME_USER_BRK_BASE` is `0x00100000`; the real value comes from the loaded
   image), the mmap region from `LUME_USER_MMAP_BASE` (`0x40000000`) and the
   stack top at `LUME_USER_STACK_TOP` (`0xBE000000`, maximum 8 MiB);
5. enter the program at `e_entry` through the same path a `fork`+`exec` would
   use, with a fresh trap frame - never by "jumping and hoping".

`ET_DYN` (PIE) and `PT_INTERP` (dynamic linking) come after the static case
works end to end, because both need a real dynamic loader and a much richer
syscall surface.

---

## 5. What exists today, and what does not

| Piece | State |
| --- | --- |
| `struct fs_node` VFS interfaces, `S_IF*` constants, `LUME_PATH_MAX` | headers written (`kernel/include/lume/fs.h`); no implementation |
| Processes, threads, `fork`-like creation, wait queues | kernel-side implementation exists (`proc.c`, `thread.c`, `sched.c`); no user-mode entry path |
| Trap frames and exception dispatch | implemented (`trapframe.h`, `vectors.S`, `exception.c`); the SVC path currently reports and kills |
| Page allocator, kernel heap, MMU section/page mapping | implemented (`mm/`, `arch/arm/mmu.c`); no per-process address spaces yet |
| User access helpers (`copy_from_user`/`copy_to_user` semantics) | implemented (`mm/uaccess.c`) |
| `/proc`-style files, devices, filesystems | not started |
| ELF loader, syscall table, signals, pipes, `futex` | not started |
| `userspace/` (init, shell, test programs) | does not exist yet |
| Build target for userspace | advertised in `make help`, not implemented - see [roadmap.md](roadmap.md) |

The honest summary: **the kernel side of process management exists, and the
user/kernel interface does not.** That is the next milestone, and it is sized as
one: get a statically linked test program to call `write(1, ...)` and
`exit(0)`.

---

## 6. Testing the compatibility layer

Three levels, matching [testing.md](testing.md):

1. **Structure layout tests (host).** `sizeof`/`offsetof` assertions for every
   ABI structure, compiled for the host with the same headers the kernel uses.
   A wrong `struct stat64` offset is caught in milliseconds this way instead of
   as a mystery in `ls`.
2. **Syscall tests under QEMU (emulated).** A tiny statically linked ARM test
   program (`userspace/`) that exercises one syscall per test and prints PASS or
   FAIL over the serial console; the QEMU runner greps the results the same way
   it greps the kernel boot markers today.
3. **Real binaries (emulated, then hardware).** Start with a static musl
   "hello world", then `busybox sh`, then a real `bash` built for ARMv6 with
   static linking. Each step is a milestone in [roadmap.md](roadmap.md), and
   each one must pass on the emulator *and* on the board before the next starts.

A dedicated `make test-userspace` target and a `userspace/` tree will appear
with the first milestone; they are deliberately not stubbed out beforehand,
because an empty directory pretending to be a userspace is exactly the kind of
"looks implemented" artefact this project avoids.
