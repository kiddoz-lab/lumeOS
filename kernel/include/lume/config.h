/*
 * LumeOS build configuration and user-visible identity strings.
 */
#ifndef LUME_CONFIG_H
#define LUME_CONFIG_H

#define LUME_NAME     "LumeOS"
#define LUME_VERSION  "0.1.0"
#define LUME_ARCH     "armv6kz"

/* The build system passes -DLUME_BUILD_COMMIT=<git describe output>.  Keeping
 * the identification in a macro (instead of __DATE__/__TIME__) keeps builds
 * reproducible. */
#ifndef LUME_BUILD_COMMIT
#define LUME_BUILD_COMMIT "unknown"
#endif
#define LUME_MACHINE  "Raspberry Pi Zero W (BCM2835)"

/* Where the second embedded program says it lives.  There is no filesystem
 * yet, so this is a name the program is told, not a path anything resolves -
 * the same fiction LUME_DEFAULT_INIT tells. */
#define LUME_MUSL_HELLO_PATH "/bin/musl-hello"

/* Strings reported through the Linux-compatible uname(2) syscall.  LumeOS is
 * not Linux, and nothing here pretends it is: the sysname is LumeOS.  The
 * kernel release is kept in the numeric form tools expect from uname. */
#define LUME_UTS_SYSNAME  "LumeOS"
#define LUME_UTS_RELEASE  "0.1.0"
#define LUME_UTS_VERSION  "LumeOS 0.1.0 (ARMv6, BCM2835, ARM EABI compatibility layer)"
#define LUME_UTS_MACHINE  "armv6l"

/* Scheduler tick.  100 Hz is a compromise that keeps idle power reasonable
 * on a 1 GHz ARM1176 while giving interactive shell latency below 20 ms. */
#define LUME_HZ 100

/* The most iovecs one writev(2) may name.  Linux's IOV_MAX is 1024; a libc's
 * stdio uses two or three, and a small cap keeps a single syscall from holding
 * the CPU for an unbounded time on a machine with one core. */
#define LUME_IOV_MAX 16

/* Kernel heap initial size (bytes) grown on demand from the page allocator. */
#define LUME_KERNEL_HEAP_INITIAL (256 * 1024)

/* Maximum number of processes/threads.  Each thread has a 4 KiB kernel
 * stack; a Pi Zero W has 512 MiB of RAM but the design targets low memory
 * use, so the table is small and statically sized to keep lookups O(1). */
#define LUME_MAX_THREADS 64
#define LUME_MAX_PROCESSES 32
#define LUME_MAX_FDS 64
#define LUME_KERNEL_STACK_SIZE 4096

/* Userspace layout (per process, 32-bit ARM). */
#define LUME_USER_BRK_BASE   0x00100000u  /* first brk after a loaded image */
#define LUME_USER_MMAP_BASE  0x40000000u  /* mmap grows upwards from here */
#define LUME_USER_STACK_TOP  0xBE000000u  /* initial stack top, 8-byte aligned */
#define LUME_USER_STACK_MAX  (8u * 1024u * 1024u)
/* Pages mapped for a fresh program even when argv/envp/auxv need less.  A
 * program that starts with a few hundred bytes of stack and immediately calls a
 * function expects the stack to grow downwards without a syscall, so the
 * initial mapping is deliberately larger than the layout. */
#define LUME_USER_STACK_MIN_PAGES 2

/* Command line passed by the Raspberry Pi firmware through cmdline.txt, and
 * also settable on the QEMU command line with -append.  LumeOS looks for
 *   lume.debug=1        verbose kernel logging
 *   lume.selftest=1     run the in-kernel self test suite at boot
 *   lume.init=/path     program to run as the first process
 */
#define LUME_DEFAULT_INIT "/bin/init"
#define LUME_DEFAULT_SHELL "/bin/sh"

#endif /* LUME_CONFIG_H */
