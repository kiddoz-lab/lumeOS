/*
 * LumeOS system call layer.
 *
 * The numbers are the *ARM Linux* numbers, because the goal is to run
 * ordinary Linux ARM binaries unmodified: a program compiled against musl or
 * glibc for armhf issues "svc #0" with the number in r7 and expects Linux's
 * semantics, its error convention (a negative return is -errno, and only
 * values in [-4095, -1] are errors) and its data structures.  Following that
 * ABI is not a shortcut around writing a real kernel - it is a compatibility
 * requirement, documented syscall by syscall in docs/userspace.md.
 *
 * The dispatch table below is deliberately a table and not a switch: an
 * unimplemented number must produce a specific, reportable failure (ENOSYS
 * and a log line naming the number), never silence and never a crash.
 */
#ifndef LUME_SYSCALL_H
#define LUME_SYSCALL_H

#include <lume/trapframe.h>
#include <lume/types.h>

/* ARM Linux (EABI) syscall numbers.  From the kernel's UAPI tables
 * (arch/arm/tools/syscall.tbl + include/uapi/asm-generic/unistd.h); see
 * docs/userspace.md section 2 for the source and for the ones not yet
 * implemented here. */
#define LUME_NR_exit          1
#define LUME_NR_fork          2
#define LUME_NR_read          3
#define LUME_NR_write         4
#define LUME_NR_open          5
#define LUME_NR_close         6
#define LUME_NR_getpid       20
#define LUME_NR_getuid       24
#define LUME_NR_getgid       47
#define LUME_NR_brk          45
#define LUME_NR_ioctl        54
#define LUME_NR_geteuid      49
#define LUME_NR_getegid      50
#define LUME_NR_dup          41
#define LUME_NR_uname       122
#define LUME_NR_wait4       114
#define LUME_NR_mmap2       192
#define LUME_NR_exit_group  248
#define LUME_NR_set_tls     0x0f0005  /* __ARM_NR_set_tls */

/* The kernel calls this from do_syscall() with the trap frame the exception
 * entry built.  Arguments are r0-r5, the number is in r7, and the return value
 * goes back in r0 - all three are properties of the ABI, not of this code. */
void syscall_dispatch(struct trapframe *tf);

/* True when LumeOS implements `nr` - the tests and the kshell use it. */
int syscall_implemented(u32 nr);

#endif /* LUME_SYSCALL_H */
