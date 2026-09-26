/*
 * LumeOS system call dispatch.
 *
 * The shape of this file is dictated by the ABI it implements: number in r7,
 * arguments in r0-r5, result in r0, and the Linux error convention (a return
 * value in [-4095, -1] is -errno).  Everything else is policy this kernel
 * chooses, and the choices are written down here rather than implied:
 *
 *   - arguments that are user pointers are validated through
 *     mm/uaccess.c, which checks the range against the live page tables and
 *     refuses anything that is not mapped for user mode.  A syscall that
 *     dereferences a bad pointer must fail with EFAULT, not fault the kernel.
 *   - an unimplemented syscall returns -ENOSYS *and* logs the number once, so
 *     bring-up of a real program reports what it needs in order, which is the
 *     fastest route to compatibility.
 *   - nothing here assumes the caller is a user process: the same path serves
 *     a kernel thread that issues a syscall during bring-up testing.
 *
 * Implemented so far: write, read, exit, exit_group, getpid, getuid/euid,
 * getgid/egid, brk, uname, wait4 and set_tls.  docs/userspace.md section 3
 * lists these against the ones a real libc needs next.
 */
#include <lume/asm.h>
#include <lume/config.h>
#include <lume/console.h>
#include <lume/errno.h>
#include <lume/fcntl.h>
#include <lume/fd.h>
#include <lume/fs.h>
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/panic.h>
#include <lume/proc.h>
#include <lume/sched.h>
#include <lume/string.h>
#include <lume/syscall.h>
#include <lume/trapframe.h>
#include <lume/types.h>

/* Kernel-side staging buffer for user memory.  A syscall that copies a large
 * buffer does it in pieces through this, so the stack stays small and the
 * range validation happens per piece. */
#define SYSCALL_COPY_CHUNK 256

/* writev(2) bounds.  Linux allows 1024 iovecs and a 2 MiB total; the console
 * cannot absorb that and the kernel is single-threaded, so the limits are small
 * enough to bound the time one call can spend and generous enough that a libc's
 * stdio never notices them (it uses two or three). */
#define SYSCALL_WRITEV_MAX_BYTES  (64u * 1024u)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/** Linux's uname(2) structure, 32-bit ARM layout (6 strings of 65 bytes). */
struct lume_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

STATIC_ASSERT(sizeof(struct lume_utsname) == 390, "utsname layout");

static struct process *caller_process(struct trapframe *tf)
{
    if ((tf->cpsr & CPSR_MODE_MASK) == MODE_USR)
        return proc_current();
    return proc_current();
}

/*
 * One range of user memory to one file descriptor.
 *
 * Copy and write in chunks: the user buffer may straddle pages, and the write
 * side (a device) may accept partial writes.  `done_io` carries the count of
 * bytes already written by earlier calls, so a short write or a fault in a
 * later range still reports what did go through - a program that sees a short
 * count retries, which is the behaviour every libc expects.
 */
static int fd_write_user(struct process *p, struct file *f, u32 user_buf, u32 len,
                         u32 *done_io)
{
    char chunk[SYSCALL_COPY_CHUNK];
    u32 done = 0;

    while (done < len) {
        u32 n = len - done;
        int written;

        if (n > sizeof(chunk))
            n = sizeof(chunk);
        if (copy_from_user(chunk, (const void *)(user_buf + done), n) < 0) {
            *done_io += done;
            return done ? (int)(*done_io) : -EFAULT;
        }
        written = f->node->ops->write(f->node, f->offset, chunk, n);
        if (written < 0) {
            *done_io += done;
            return done ? (int)(*done_io) : written;
        }
        f->offset += (u32)written;
        done += (u32)written;
        if ((u32)written < n)
            break;   /* short write: report what went through */
    }
    *done_io += done;
    return (int)*done_io;
}

static int sys_write(struct trapframe *tf)
{
    int fd = (int)tf->r[0];
    u32 user_buf = tf->r[1];
    u32 len = tf->r[2];
    struct process *p = caller_process(tf);
    struct file *f;
    u32 done = 0;

    f = fd_get(p, fd);
    if (!f || !f->node || !f->node->ops || !f->node->ops->write)
        return -EBADF;
    if (len == 0)
        return 0;
    return fd_write_user(p, f, user_buf, len, &done);
}

/*
 * writev(2) - a vector of buffers to one descriptor.
 *
 * This is not a convenience wrapper: musl's stdio writes *through* writev
 * (`src/stdio/__stdio_write.c`), so without it a program built against a real C
 * library produces no output at all, and printf does not fail loudly - it
 * buffers, the flush returns an error nobody reads, and the process exits 0
 * with an empty console.  That is a specific enough failure to be worth naming
 * here, because "the program ran and printed nothing" is otherwise a mystery.
 *
 * The kernel copies each iovec out of user memory itself (see sys_write) so a
 * buffer that straddles a page boundary or an iovec pointing at unmapped memory
 * is refused, not followed.  Linux would write the ranges straight from user
 * memory; copying is the difference between a buffer that is validated and one
 * that is trusted, and on this kernel the console is not fast enough to care.
 */
static int sys_writev(struct trapframe *tf)
{
    int fd = (int)tf->r[0];
    u32 user_iov = tf->r[1];
    int count = (int)tf->r[2];
    struct process *p = caller_process(tf);
    struct file *f;
    u32 done = 0;
    u32 total = 0;

    f = fd_get(p, fd);
    if (!f || !f->node || !f->node->ops || !f->node->ops->write)
        return -EBADF;
    if (count < 0)
        return -EINVAL;
    if (count == 0)
        return 0;
    if (count > LUME_IOV_MAX)
        return -EINVAL;

    for (int i = 0; i < count; i++) {
        struct lume_iovec iov;
        int rc;

        if (copy_from_user(&iov, (const void *)(user_iov + (u32)i * sizeof(iov)),
                           sizeof(iov)) < 0) {
            done += total;
            return done ? (int)done : -EFAULT;
        }
        if (iov.len == 0)
            continue;
        if (total + iov.len > SYSCALL_WRITEV_MAX_BYTES) {
            done += total;
            return done ? (int)done : -EINVAL;
        }
        total += iov.len;
        rc = fd_write_user(p, f, iov.base, iov.len, &done);
        if (rc < 0)
            return rc;
        if ((u32)rc < total)   /* short write: stop, report what went through */
            break;
    }
    return (int)done;
}

static int sys_read(struct trapframe *tf)
{
    int fd = (int)tf->r[0];
    u32 user_buf = tf->r[1];
    u32 len = tf->r[2];
    struct process *p = caller_process(tf);
    struct file *f;
    char chunk[SYSCALL_COPY_CHUNK];
    int n;

    f = fd_get(p, fd);
    if (!f || !f->node || !f->node->ops || !f->node->ops->read)
        return -EBADF;
    if (len == 0)
        return 0;

    if (len > sizeof(chunk))
        len = sizeof(chunk);

    n = f->node->ops->read(f->node, f->offset, chunk, len);
    if (n < 0)
        return n;
    if (copy_to_user((void *)user_buf, chunk, (u32)n) < 0)
        return -EFAULT;
    f->offset += (u32)n;
    return n;
}

static int sys_brk(struct trapframe *tf)
{
    struct process *p = caller_process(tf);
    u32 requested = tf->r[0];

    if (!p || !p->as)
        return -ENOMEM;

    /* brk(0) asks for the current break - the one case where the ABI's
     * argument is a query rather than a request. */
    if (requested == 0)
        return (int)p->brk;

    /* First call after exec: the kernel placed the break at the end of the
     * loaded image, which is the floor it may never shrink below. */
    if (!p->brk)
        p->brk = p->brk_base;

    if (requested < p->brk) {
        /* Shrinking is allowed but only down to the initial break: the loader
         * mapped the image, and those pages are not the heap's to give back. */
        if (requested < p->brk_base)
            return (int)p->brk;
        for (u32 va = PAGE_ALIGN_UP(requested); va < PAGE_ALIGN_UP(p->brk); va += PAGE_SIZE)
            vmm_unmap_page(p->as, va);
        p->brk = requested;
        return (int)p->brk;
    }

    if (requested > LUME_USER_MMAP_BASE) {
        /* Refuse to grow the heap into the mmap area rather than corrupting
         * whatever is mapped there. */
        return (int)p->brk;
    }

    for (u32 va = PAGE_ALIGN_UP(p->brk); va < PAGE_ALIGN_UP(requested); va += PAGE_SIZE) {
        u32 pa = pmm_alloc_page();

        if (!pa)
            return (int)p->brk;
        if (vmm_map_page(p->as, va, pa, VM_FLAG_USER | VM_FLAG_WRITE) < 0) {
            pmm_free_page(pa);
            return (int)p->brk;
        }
        /* Fresh heap pages must be zeroed: a program is entitled to assume
         * that the memory it gets from brk has never held anything else. */
        memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
    }
    p->brk = requested;
    return (int)p->brk;
}

static int sys_uname(struct trapframe *tf)
{
    struct lume_utsname u;
    u32 user_ptr = tf->r[0];

    memset(&u, 0, sizeof(u));
    strncpy(u.sysname, LUME_UTS_SYSNAME, sizeof(u.sysname) - 1);
    strncpy(u.nodename, "lumeos", sizeof(u.nodename) - 1);
    strncpy(u.release, LUME_UTS_RELEASE, sizeof(u.release) - 1);
    strncpy(u.version, LUME_UTS_VERSION, sizeof(u.version) - 1);
    strncpy(u.machine, LUME_UTS_MACHINE, sizeof(u.machine) - 1);
    strncpy(u.domainname, "(none)", sizeof(u.domainname) - 1);

    if (copy_to_user((void *)user_ptr, &u, sizeof(u)) < 0)
        return -EFAULT;
    return 0;
}

static int sys_wait4(struct trapframe *tf)
{
    struct process *p = caller_process(tf);
    u32 status_out = tf->r[1];
    u32 options = tf->r[2];
    u32 status = 0;
    int pid;

    pid = proc_wait(p, (s32)tf->r[0], options, &status);
    if (pid < 0)
        return pid;
    if (status_out && copy_to_user((void *)status_out, &status, sizeof(status)) < 0)
        return -EFAULT;
    return pid;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

/* Called for every number the kernel does not implement, once per number, so
 * that bring-up of a real program reports its requirements in order without
 * flooding the console. */
static void report_unimplemented(u32 nr)
{
    static u32 reported[8];
    static u32 reported_count;

    for (u32 i = 0; i < reported_count; i++)
        if (reported[i] == nr)
            return;
    if (reported_count < sizeof(reported) / sizeof(reported[0])) {
        reported[reported_count++] = nr;
        pr_warn("syscall: %u is not implemented (returning ENOSYS)", nr);
    } else {
        pr_warn("syscall: %u is not implemented (further reports suppressed)", nr);
    }
}

int syscall_implemented(u32 nr)
{
    switch (nr) {
    case LUME_NR_write:
    case LUME_NR_read:
    case LUME_NR_exit:
    case LUME_NR_exit_group:
    case LUME_NR_getpid:
    case LUME_NR_getuid:
    case LUME_NR_geteuid:
    case LUME_NR_getgid:
    case LUME_NR_getegid:
    case LUME_NR_brk:
    case LUME_NR_uname:
    case LUME_NR_wait4:
    case LUME_NR_writev:
    case LUME_NR_set_tid_address:
    case LUME_NR_set_tls:
        return 1;
    default:
        return 0;
    }
}

void syscall_dispatch(struct trapframe *tf)
{
    u32 nr = tf->r[7];

    switch (nr) {
    case LUME_NR_write:
        tf->r[0] = (u32)sys_write(tf);
        return;
    case LUME_NR_read:
        tf->r[0] = (u32)sys_read(tf);
        return;
    case LUME_NR_getpid:
        tf->r[0] = proc_current() ? proc_current()->pid : 0;
        return;
    case LUME_NR_getuid:
    case LUME_NR_geteuid:
        tf->r[0] = proc_geteuid();
        return;
    case LUME_NR_getgid:
    case LUME_NR_getegid:
        tf->r[0] = proc_getegid();
        return;
    case LUME_NR_brk:
        tf->r[0] = (u32)sys_brk(tf);
        return;
    case LUME_NR_uname:
        tf->r[0] = (u32)sys_uname(tf);
        return;
    case LUME_NR_wait4:
        tf->r[0] = (u32)sys_wait4(tf);
        return;
    case LUME_NR_writev:
        tf->r[0] = (u32)sys_writev(tf);
        return;
    case LUME_NR_set_tid_address: {
        /* The kernel picks the pid, so the argument is only the address a
         * thread's exit should clear - which belongs to futexes and threads,
         * neither of which exists yet.  What a caller observes today is the
         * return value, and that is the pid, as on Linux. */
        struct process *p = caller_process(tf);

        if (!p)
            return;
        p->clear_child_tid = tf->r[0];
        tf->r[0] = p->pid;
        return;
    }
    case LUME_NR_set_tls: {
        struct process *p = caller_process(tf);

        if (p)
            p->tls = tf->r[0];
        /* The hardware register the kernel keeps in sync is written when an
         * address space switch happens; see arch/arm/context.c. */
        arm_write_tpidruro(tf->r[0]);
        tf->r[0] = 0;
        return;
    }
    case LUME_NR_exit:
    case LUME_NR_exit_group: {
        struct process *p = caller_process(tf);
        int code = (int)(tf->r[0] & 0xFF);

        if (!p)
            panic("exit syscall with no current process");
        pr_info("syscall: pid %u exited via %s(%d)",
                p->pid, nr == LUME_NR_exit ? "exit" : "exit_group", code);
        proc_exit(p, code);
        return;   /* not reached */
    }
    default:
        report_unimplemented(nr);
        tf->r[0] = (u32)(-ENOSYS);
        return;
    }
}
