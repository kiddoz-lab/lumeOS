/*
 * LumeOS userspace: the ARM Linux system call ABI, called by hand.
 *
 * These are the *Linux ARM EABI* syscall numbers and the *Linux* calling
 * convention - number in r7, arguments in r0-r5, result in r0, entered with
 * "svc #0" - because that convention is the whole point of the exercise: a
 * static ARM Linux binary built by an ordinary compiler issues exactly these
 * instructions with exactly these numbers, so a program that uses this header
 * is a program LumeOS must be able to run, and vice versa.
 *
 * The numbers are from the Linux kernel's UAPI headers
 * (arch/arm/tools/syscall.tbl, unistd-eabi.h); see docs/userspace.md section 2
 * for the table and for how errors are reported (a negative value in r0 is
 * -errno, exactly like Linux).
 */
#ifndef LUME_USER_SYSCALL_H
#define LUME_USER_SYSCALL_H

/* ARM Linux syscall numbers (EABI).  Only the ones this program needs, plus a
 * few that document the direction of travel. */
#define LUME_SYS_exit        1
#define LUME_SYS_write       4
#define LUME_SYS_getpid     20
#define LUME_SYS_brk        45
#define LUME_SYS_uname     122
#define LUME_SYS_writev    146
#define LUME_SYS_exit_group 248
#define LUME_SYS_set_tid_address 256

#ifndef __ASSEMBLER__
#ifndef __ASSEMBLY__

/* Six arguments is the ARM Linux maximum (r0-r5 after the number in r7). */
static inline long lume_syscall0(long n)
{
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0");

    __asm__ __volatile__("svc #0" : "=r"(r0) : "r"(r7) : "memory");
    return r0;
}

static inline long lume_syscall1(long n, long a)
{
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;

    __asm__ __volatile__("svc #0" : "+r"(r0) : "r"(r7) : "memory");
    return r0;
}

static inline long lume_syscall2(long n, long a, long b)
{
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;

    __asm__ __volatile__("svc #0" : "+r"(r0) : "r"(r7), "r"(r1) : "memory");
    return r0;
}

static inline long lume_syscall3(long n, long a, long b, long c)
{
    register long r7 __asm__("r7") = n;
    register long r0 __asm__("r0") = a;
    register long r1 __asm__("r1") = b;
    register long r2 __asm__("r2") = c;

    __asm__ __volatile__("svc #0" : "+r"(r0) : "r"(r7), "r"(r1), "r"(r2) : "memory");
    return r0;
}

static inline long lume_write(int fd, const void *buf, unsigned long len)
{
    return lume_syscall3(LUME_SYS_write, fd, (long)buf, (long)len);
}

static inline long lume_getpid(void)
{
    return lume_syscall0(LUME_SYS_getpid);
}

/* struct iovec, as the ABI defines it: two 32-bit fields, base then length.
 * Spelled out rather than pulled from a libc header because this program has no
 * libc - the point of the header is that the numbers and the layout are the
 * kernel's, not somebody's idea of them. */
struct lume_iovec {
    void *base;
    unsigned long len;
};

static inline long lume_writev(int fd, const struct lume_iovec *iov, int count)
{
    return lume_syscall3(LUME_SYS_writev, fd, (long)iov, count);
}

static inline long lume_set_tid_address(long tidptr)
{
    return lume_syscall1(LUME_SYS_set_tid_address, tidptr);
}

static inline void lume_exit(int code)
{
    lume_syscall1(LUME_SYS_exit, code);
    /* exit(2) does not return; if it does, try the group exit and then spin,
     * because there is nothing sensible left to do. */
    lume_syscall1(LUME_SYS_exit_group, code);
    for (;;)
        ;
}

#endif /* !__ASSEMBLY__ */
#endif /* !__ASSEMBLER__ */
#endif /* LUME_USER_SYSCALL_H */
