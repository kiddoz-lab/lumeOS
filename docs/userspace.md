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
init: loading the embedded 28244-byte init image (entry 0x000100b4, sha256 ...)
init: stack at 0xbdffff20..0xbe000000 (2 pages, 224 bytes: argc/argv/envp + 17 auxv entries)
init: auxv AT_PAGESZ 4096, AT_ENTRY 0x000100b4, AT_PHDR 0x00010034, AT_PHNUM 3, AT_HWCAP 0x00008097, AT_CLKTCK 100
init: entering user mode at 0x000100b4 on stack 0xbdffff20
init: hello from user mode
init: pid 1, argc 1
init: argv[0] is "/bin/init"
init: this line went to file descriptor 2
init: auxv at 0xbdffff30: AT_PAGESZ 4096, AT_ENTRY 0x000100b4, AT_PHDR 0x00010034, AT_PHNUM 3, AT_HWCAP 0x00008097, AT_CLKTCK 100
init: auxv AT_PHNUM 3 headers, 2 PT_LOAD, AT_PHDR 0x00010034 (in a segment), entry is executable
init: auxv AT_RANDOM 16 bytes, seed byte 0xNN (not all zero)
init: auxv verified
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

### The initial stack and the auxiliary vector

A program is started with a stack the kernel built, and the shape of that stack
is the interface: a C library reads it before `main` runs and has no other way
to learn the page size, where its program headers are, or what the CPU may be
asked to do. The layout is Linux's, lowest address first:

```
sp -> [ argc ][ argv[0..argc-1] ][ NULL ][ envp[0..envc-1] ][ NULL ]
      [ auxv: type, value, type, value, ... ][ AT_NULL, 0 ]
      (the strings those pointers name, and 16 bytes AT_RANDOM points at)
```

`kernel/kernel/ustack.c` builds it, and two properties are worth naming because
they are easy to get wrong and impossible to notice afterwards: **sp is rounded
to 16 bytes** (the ARM ABI needs 8; Linux rounds to 16 and some runtimes assume
it), and **the strings sit above the tables** that point at them, because the
stack grows down and they are pushed first.

What LumeOS writes into `auxv` today:

| Entry | Value | Why it is there |
| --- | --- | --- |
| `AT_HWCAP` | `0x00008097` | what the CPU may be asked to do - see below |
| `AT_PAGESZ` | `4096` | `PAGE_SIZE`; memory mapping arithmetic starts here |
| `AT_CLKTCK` | `100` | what `times(2)` counts in (Linux's `USER_HZ`, and our `LUME_HZ`) |
| `AT_PHDR`, `AT_PHENT`, `AT_PHNUM` | `0x00010034`, `32`, `3` | where the program's own header *table* is, so a libc can find `PT_TLS`, `PT_GNU_RELRO` and `PT_GNU_STACK` without opening a file it may not have |
| `AT_BASE` | `0` | the interpreter's address: there is no dynamic linker, and this is how a static program says so |
| `AT_FLAGS` | `0` | nothing set, nothing reserved |
| `AT_ENTRY` | `0x000100b4` | where the program started (the entry the loader was given, not a number the stack builder guesses) |
| `AT_UID`/`AT_EUID`/`AT_GID`/`AT_EGID` | the process's ids | `0` today (no filesystem, no `setuid`), but read from the process, not hardcoded |
| `AT_SECURE` | `0` | no privilege boundary has been crossed, so nothing needs to be distrusted |
| `AT_RANDOM` | 16 bytes on the stack | the C library's stack canary seed |
| `AT_EXECFN` | the name the program was started with | `/bin/init` today; survives a program rewriting its own `argv` |
| `AT_PLATFORM` | `"v6l"` | Linux's `elf_name` for an ARMv6 core plus the endianness letter; used for `/lib/<platform>/` lookup |
| `AT_NULL` | `0` | terminates the vector - a program looking for a type that is not there walks until it finds this |

Two absences are decisions rather than gaps:

* **`AT_HWCAP` never has a floating-point bit set.** The BCM2835's ARM1176 does
  have VFPv2 and Linux would set `HWCAP_VFP` - but Linux also saves and restores
  the FP registers on every context switch, and LumeOS does not. A program that
  took the bit at its word would find its floating-point state clobbered by
  whatever ran in between, which is worse than not running at all. So the mask
  is `SWP | HALF | THUMB | FAST_MULT | EDSP | TLS`: Linux's `proc-v6.S` list
  minus `HWCAP_JAVA` (the kernel never enables the Jazelle state, so it will not
  invite a JVM to use it) minus every FP bit. A `static_assert` in
  `kernel/include/lume/auxv.h` fails the build if anyone widens it by accident,
  and the program checks the bit is clear before it continues.
* **`AT_HWCAP2` is not written at all**, so it reads as 0. That is the right
  answer for ARMv6, where every bit it defines (IDIV, LPAE, the crypto
  extensions) belongs to a later architecture.

`HWCAP_TLS` is set, and that one is a promise the kernel keeps:
ARM1176JZF-S is ARMv6K, so `TPIDRURO` exists, `set_tls` stores a per-process
value for it, and `arch/arm/context.c` reprograms the register on every switch
to a user thread.

`AT_RANDOM` deserves its own paragraph, because it is a security mechanism and
the kernel's honesty about it is the point. The 16 bytes come from
`kernel/kernel/random.c`: an xorshift32 generator seeded from the system timer,
the tick count and the kernel's own load address. That is enough to make a
canary differ between processes and between boots, and it is **not** enough to
call cryptographic entropy - an attacker who learns one process's canary can
predict the next one's. The fix is a real source, and the BCM2835 has one (a
hardware RNG at `0x20104000`, which QEMU models); driving it is a listed task in
[roadmap.md](roadmap.md) rather than a claim made here.

Seeding *replaces* the state rather than mixing into it, so the same seed
replays the same stream. That is a deliberate property with a test behind it:
the self test seeds the generator, records 16 bytes, reseeds with the same
value, and compares - which is how it proves that the bytes `AT_RANDOM` points
at in real user memory are the generator's output and not whatever the page
happened to contain. A generator that folded the previous state into every seed
could not be checked that way. Callers who want more entropy pass it in the
seed: `lume_random_init()` mixes the microsecond counter, the tick count, the
kernel's load address and the pid across two calls.

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
   with `sp` rounded to 16 bytes - the EABI needs 8, Linux rounds to 16, and
   some C runtimes assume it;
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

`ET_DYN` (PIE), `PT_INTERP` (dynamic linking) and `exec()` come after the static
case works end to end: PIE needs relocation processing, and the dynamic loader
needs a filesystem to read from.

### One thing the loader requires of the image: the headers must be mapped

`AT_PHDR` is not a file offset. A program is handed *an address* and told it
points at its own program header table, and the loader computes it the way Linux
does: find the `PT_LOAD` whose file range contains `e_phoff` and report

```
AT_PHDR = e_phoff - p_offset + p_vaddr        (of that segment)
```

If no segment contains the headers they are not in memory at all, and the honest
value is `0` - the convention for "this is not available". That is a state a C
library cannot start in, because reading the program headers is how it finds
`PT_TLS`, `PT_GNU_RELRO` and `PT_GNU_STACK` before it runs a line of the
program.

So the userspace linker script has to put the headers inside the first segment,
which is what `userspace/init/linker.ld` does with `FILEHDR PHDRS` on the text
segment and `. = 0x00010000 + SIZEOF_HEADERS`. This is not obvious, it is
invisible in `readelf -l` output, and getting it wrong is how init first died:
the text segment began at file offset `0x1000` with the headers unmapped, the
kernel dutifully reported `AT_PHDR = 0x00000034` (the null page), and the
program that went to read its own headers was killed by the SIGSEGV handler it
did not have. Three things now stand in the way of a repeat, at three different
costs: `tools/embed_user.py` refuses such an image at build time, the kernel
self test loads the image and reads the table back through the target address
space, and `init` itself checks that `AT_PHDR` lies inside a `PT_LOAD` and that
`AT_ENTRY` lies inside one marked executable.

What a program may assume *today*: `argc`/`argv`/`envp`, the auxiliary vector
above, and `sp` 16-byte aligned at entry. There is still no `PT_INTERP` handling,
so a dynamically linked binary does not start (§5).

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
| Initial stack | ✅ `argc`, `argv[]`, `envp`, both `NULL`-terminated, `sp` rounded to **16** bytes at entry; the environment is deliberately empty (there is no init script and no PATH to set), and a non-empty one is covered by the kernel self test |
| `auxv` | ✅ 17 pairs plus `AT_NULL`: `AT_HWCAP`, `AT_PAGESZ`, `AT_CLKTCK`, `AT_PHDR`/`AT_PHENT`/`AT_PHNUM`, `AT_BASE`, `AT_FLAGS`, `AT_ENTRY`, the four ids, `AT_SECURE`, `AT_RANDOM`, `AT_EXECFN`, `AT_PLATFORM`; the program checks the values against its own copy of the numbers and prints `init: auxv verified` (a required marker in QEMU) - the table above lists each one |
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
  markers `init: hello from user mode`, `init: auxv verified` and
  `init: exiting with status 0` come from user mode through `write(2)`, and
  `init: pid 1 exited with status 0` comes from the kernel reaping it. A build
  that boots but no longer runs programs - or hands one a broken stack - fails
  the build.
* **`tests/host/test_ustack_layout.py`** runs the stack builder against a fake
  machine, so the layout arithmetic (order, alignment, terminators, auxv values,
  and the refusal paths) is covered on any development machine without an
  emulator; the in-kernel self test covers the same ground against the real page
  tables, and `init`'s checks cover the stack a program actually receives.

What is *not* in place: a `make test-userspace` target, a test program that
exercises one syscall per case, and the structure-layout tests from level 1
above. Those arrive with the syscall surface they would test - an assertion
suite for `struct stat64` is not useful while `stat64` does not exist.
