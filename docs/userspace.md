# Userspace and the ARM Linux ABI

The long-term goal of LumeOS is to run ordinary 32-bit ARM Linux ELF
executables **unmodified**, and eventually to build and run a real `bash`. That
is a compatibility target, not a feature: it means implementing the syscall
surface, the memory semantics, the process model, the signal behaviour and the
filesystem semantics those binaries depend on. An ELF parser alone gets you a
program that faults on its first `write()`.

This document describes the compatibility plan, the ABI facts the
implementation is built against, and - just as importantly - what exists today.

**Today, userspace exists and runs.** The kernel loads a static 32-bit ARM ELF,
maps it into a fresh address space, builds the Linux entry stack, enters ARM
user mode, dispatches the program's `svc #0` calls, and reaps it when it exits.
The first program (`userspace/init/main.c`) prints this, and CI asserts every
line of it:

```
init: loading the embedded 25504-byte init image (entry 0x00010000, sha256 ...)
init: entering user mode at 0x00010000 on stack 0xbdffffe4 (1 segments, 1 pages)
init: hello from user mode
init: pid 1, argc 1
init: argv[0] is "/bin/init"
init: this line went to file descriptor 2
init: exiting with status 0
init: pid 1 exited with status 0 (exit code 0)
```

Two of those lines are produced by the program itself through `write(2)`, so
they can only exist if user mode, the syscall instruction, the dispatcher, the
console device and the return path all work. The rest of this document is what
that implementation does and does not cover - §5 is the honest inventory.

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

The shape in use today (and the shape the native personality will extend rather
than replace):

```
 userspace binary
        |  svc #0, nr in r7
        v
 arch/arm/vectors.S          -> the SVC vector, saves a struct trapframe
        |
        v
 arch/arm/exception.c        -> do_syscall(): recognises the self-test probe,
        |                       hands everything else to the dispatcher
        v
 kernel/kernel/syscall.c     -> syscall_dispatch(): the ARM Linux numbers, the
        |                       Linux error convention, one case per call
        v
 kernel services             -> fd table, console device, address spaces (vmm),
                               physical allocator, processes/threads, scheduler
```

The separate `kernel/syscalls/linux.c` + `native.c` split described earlier in
this document is still the plan for the native personality; it is *not* what
exists, and pretending otherwise in the file layout would be worse than saying
so. One file, one switch, one table of numbers - with the numbers transcribed
from the UAPI headers and recorded in §2 - is enough until the native calls
arrive, at which point the split becomes a mechanical change.

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

What the loader in `kernel/kernel/elf.c` does today, in order, with the
refusals it makes by name:

| Step | Behaviour |
| --- | --- |
| Header check | `ELFCLASS32`, `ELFDATA2LSB`, `EM_ARM`, `ET_EXEC`. Anything else is refused with a reason string, printed by the kernel: a PIE is refused as *"ET_DYN (PIE/shared object) needs a relocation loader"* rather than failing later at address 0 |
| Bounds | every `p_offset`/`p_filesz` is checked against the image size, and every segment against the user address range `0x00010000..0xB0000000`, so a truncated or hostile image cannot fault the kernel mid-copy |
| Mapping | one page at a time, `p_flags` → `VM_FLAG_USER/WRITE/EXEC`; a page shared by two segments is mapped **once** and written twice (legal, and what the linker produces when `.text` and `.rodata` land in one page) |
| `bss` | the remainder of every segment is zeroed before mapping, so the page never exposes another program's data |
| Coherence | the data cache is cleaned and the instruction cache invalidated for the mapped range - ARMv6 has separate I and D caches, and without this the CPU can execute whatever the I-cache held for that physical page |
| Failure | a failed load unmaps and frees everything it mapped, so the caller cannot leak a half-loaded image |

`ET_DYN` (PIE), `PT_INTERP` (dynamic linking), `auxv` and `exec()` come after the
static case works end to end: PIE needs relocation processing, and the dynamic
loader needs a filesystem to read from. What a program may assume *today* is
`argc`/`argv` on a valid stack and nothing else - in particular there is **no
`auxv`**, which is why a glibc binary will not start yet (see §5).

---

## 5. What exists today, and what does not

Everything in this table is either something a CI run has shown or something the
code does not contain. Nothing here is aspirational.

| Piece | State |
| --- | --- |
| ELF32 loader (`kernel/kernel/elf.c`) | ✅ loads, maps and enters a static `EM_ARM` `ET_EXEC`; refuses PIE, foreign architectures, truncated images and out-of-range segments by name; covered by 7 in-kernel checks against the real embedded image |
| Syscall entry and dispatch (`arch/arm/exception.c`, `kernel/kernel/syscall.c`) | ✅ `svc #0`, number in `r7`, arguments `r0-r5`, result in `r0`, Linux error convention |
| Implemented syscalls | `write 4`, `read 3`, `exit 1`, `exit_group 248`, `getpid 20`, `getuid 24`, `geteuid 49`, `getgid 47`, `getegid 50`, `brk 45`, `uname 122`, `wait4 114`, `set_tls 0x0f0005` |
| Unimplemented syscalls | return `-ENOSYS` (38) and are logged once per number, naming the number. They do not kill the caller: a program that gets a proper error is a program we can still learn from |
| User address spaces | ✅ per process, with the kernel half shared; the loader maps into the new space and the scheduler switches to it |
| `brk` heap | ✅ real: pages are allocated, mapped user-writable and **zeroed** on request; `brk(0)` reports the current break; growth is refused before the `mmap` region (`0x40000000`) instead of corrupting it; shrinking stops at the end of the loaded image |
| Initial stack | ✅ `argc`, `argv[]`, a terminating `NULL`, an empty `envp`; 8-byte aligned at entry |
| `auxv` | ⛔ not written. A libc that needs `AT_HWCAP`/`AT_PAGESZ` will not start |
| Console as a device | ✅ `fs_node` + ops table (`kernel/drivers/console.c`); descriptors 0/1/2 point at it; `write` does CRLF translation; `read` blocks in the input core's wait queue; `ioctl` returns `-ENOTTY` because there is no terminal driver yet |
| Process model | ✅ one thread per process; exit status, zombie state, parent wake-up, `wait4` with `WNOHANG`, and the thread slot plus its kernel stack are returned to the pool (a process that exits 64 times does not run out of either) |
| File descriptors | ✅ table, `refcount` sharing, `dup`/`dup2`/`fd_inherit` for a future `fork` |
| VFS, `/proc`, block devices, filesystems | ⛔ only the interfaces in `kernel/include/lume/fs.h`. There is no path lookup, so `open`/`openat` cannot be implemented meaningfully yet, and the first program is embedded in the kernel image rather than read from a card |
| `mmap2`/`munmap`/`mprotect` | ⛔ not implemented (the `vmm` calls they need exist; the syscalls and their accounting do not) |
| Signals, `clone`/`fork`, `futex`, pipes, `clock_gettime` | ⛔ not implemented |
| `userspace/` | ✅ `init/main.c`, a freestanding `crt0.S`, a linker script, and `lib/lume/syscall.h`; built by `make userspace`, embedded by `tools/embed_user.py`, and asserted end-to-end by the QEMU test |
| `exec()` | ⛔ the embedded image is loaded once at boot; there is no way to replace a running process's image |

The honest summary: **a static ARM ELF runs, makes syscalls and exits, and the
program that does it is the only program there is.** The gap between that and
"runs ordinary ARM Linux binaries" is the list in §1, and the order it gets
closed in is in [roadmap.md](roadmap.md).

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

### What is in place now

* **`make userspace`** builds `build/userspace/init.elf` with the same
  ARMv6 flags as the kernel, and the ISA and EABI gates run on it too
  (`check_abi.py --allow-none`, because a `-nostdlib` program that never calls a
  division helper legitimately defines none - if it ever does define one, its
  layout is checked like the kernel's).
* **`tools/embed_user.py`** validates the image where a mistake is cheapest to
  find - at build time - and has its own host tests (`tests/host/test_embed_user.py`)
  covering each refusal: a foreign architecture, `ET_DYN`, a 64-bit image, a
  non-ELF, a truncated file and an entry point inside the null page.
* **The in-kernel self tests** validate the loader against the image the kernel
  is actually carrying, including four mutated headers that must be refused.
* **The QEMU test asserts the program's output**, not just the kernel's: the
  markers `init: hello from user mode` and `init: exiting with status 0` come
  from user mode through `write(2)`, and `init: pid 1 exited with status 0`
  comes from the kernel reaping it. A build that boots but no longer runs
  programs fails the build.

What is *not* in place: a `make test-userspace` target, a test program that
exercises one syscall per case, and the structure-layout tests from level 1
above. Those arrive with the syscall surface they would test - an assertion
suite for `struct stat64` is not useful while `stat64` does not exist.
