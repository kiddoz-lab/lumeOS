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

static int sys_write(struct trapframe *tf)
{
    int fd = (int)tf->r[0];
    u32 user_buf = tf->r[1];
    u32 len = tf->r[2];
    struct process *p = caller_process(tf);
    struct file *f;
    char chunk[SYSCALL_COPY_CHUNK];
    u32 done = 0;

    f = fd_get(p, fd);
    if (!f || !f->node || !f->node->ops || !f->node->ops->write)
        return -EBADF;

    if (len == 0)
        return 0;

    /* Copy and write in chunks: the user buffer may straddle pages, and the
     * write side (a device) may accept partial writes. */
    while (done < len) {
        u32 n = len - done;

        if (n > sizeof(chunk))
            n = sizeof(chunk);
        if (copy_from_user(chunk, (const void *)(user_buf + done), n) < 0)
            return done ? (int)done : -EFAULT;
        {
            int written = f->node->ops->write(f->node, f->offset, chunk, n);

            if (written < 0)
                return done ? (int)done : written;
            f->offset += (u32)written;
            done += (u32)written;
            if ((u32)written < n)
                break;   /* short write: report what went through */
        }
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
