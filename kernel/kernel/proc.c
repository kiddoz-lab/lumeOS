/*
 * LumeOS process management.
 */
#include <lume/asm.h>
#include <lume/fs.h>
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/panic.h>
#include <lume/errno.h>
#include <lume/proc.h>
#include <lume/sched.h>
#include <lume/string.h>
#include <lume/time.h>
#include <lume/types.h>

static struct process processes[LUME_MAX_PROCESSES];
static struct process *current;
static u32 next_pid = 1;

/* Kernel-internal file objects live in fd.c. */
void fd_init_process(struct process *p);

struct process *proc_current(void)
{
    return current;
}

void proc_set_current(struct process *p)
{
    current = p;
}

struct process *proc_by_pid(u32 pid)
{
    for (u32 i = 0; i < LUME_MAX_PROCESSES; i++) {
        if (processes[i].state != PROC_STATE_UNUSED && processes[i].pid == pid)
            return &processes[i];
    }
    return NULL;
}

u32 proc_alloc_pid(void)
{
    return next_pid++;
}

void proc_init(void)
{
    memset(processes, 0, sizeof(processes));
    next_pid = 1;

    /* Process 0 is the kernel itself: pid 0, kernel address space, cwd "/". */
    current = &processes[0];
    current->pid = 0;
    current->ppid = 0;
    current->state = PROC_STATE_ALIVE;
    current->kind = PROC_IDLE;
    current->as = vmm_kernel_space();
    current->uid = current->euid = 0;
    current->gid = current->egid = 0;
    strcpy(current->cwd, "/");
    fd_init_process(current);
    wait_queue_init(&current->waitq, "proc-exit");
    wait_queue_init(&current->sigq, "proc-sigchld");

    pr_info("proc: kernel process pid 0, cwd %s", current->cwd);
}

struct process *proc_create(const char *name, u32 entry, u32 user_sp, u32 arg,
                            struct process *parent)
{
    struct process *p = NULL;
    struct thread *t;

    u32 flags = arm_irq_save();

    for (u32 i = 1; i < LUME_MAX_PROCESSES; i++) {
        if (processes[i].state == PROC_STATE_UNUSED) {
            p = &processes[i];
            break;
        }
    }
    if (p) {
        struct process *init = p;
        memset(init, 0, sizeof(*init));
        init->state = PROC_STATE_ALIVE;
        init->kind = PROC_USER;
        init->pid = proc_alloc_pid();
        init->ppid = parent ? parent->pid : 0;
        init->pgid = parent ? parent->pgid : init->pid;
        init->sid = parent ? parent->sid : init->pid;
        init->uid = init->euid = parent ? parent->euid : 0;
        init->gid = init->egid = parent ? parent->egid : 0;
        init->parent = parent;
        strcpy(init->cwd, parent ? parent->cwd : "/");
        wait_queue_init(&init->waitq, "proc-exit");
        wait_queue_init(&init->sigq, "proc-sigchld");
    }
    if (!p) {
        arm_irq_restore(flags);
        return NULL;
    }

    p->as = vmm_space_create();
    if (!p->as) {
        p->state = PROC_STATE_UNUSED;
        arm_irq_restore(flags);
        return NULL;
    }

    /* Inherit the parent's open files, exactly like fork(2). */
    fd_init_process(p);
    if (parent) {
        extern int fd_inherit(struct process *child, struct process *parent);
        fd_inherit(p, parent);
    }

    if (parent) {
        for (u32 i = 0; i < LUME_MAX_PROCESSES; i++) {
            if (!parent->children[i]) {
                parent->children[i] = p;
                break;
            }
        }
    }

    t = thread_create_user(p, name, entry, user_sp, arg);
    if (!t) {
        vmm_space_destroy(p->as);
        p->state = PROC_STATE_UNUSED;
        arm_irq_restore(flags);
        return NULL;
    }
    p->thread = t;
    p->threads = 1;

    arm_irq_restore(flags);
    return p;
}

void proc_note_thread_exit(struct process *p, int code)
{
    if (!p)
        return;
    if (p->threads > 0)
        p->threads--;
    if (p->threads == 0)
        p->exit_code = code & 0xFF;
}

void proc_exit(struct process *p, int code)
{
    struct process *parent;

    if (!p)
        p = current;
    if (!p)
        panic("proc_exit: no current process");

    p->exit_code = code & 0xFF;
    p->state = PROC_STATE_ZOMBIE;

    pr_debug("proc: pid %u exited with status %d", p->pid, p->exit_code);

    /* Hand the exit status to the parent (or to init, pid 1). */
    parent = p->parent;
    if (parent && parent != p)
        wait_queue_wake_all(&parent->waitq);

    /* p->thread is the running thread; it cleans up and switches away. */
    thread_exit(code);
}

int proc_wait(struct process *parent, s32 pid, u32 options, u32 *status_out)
{
    u32 flags;
    int ret = -1;

    if (!parent)
        parent = current;
    if (!parent)
        return -1;

    for (;;) {
        struct process *child = NULL;

        flags = arm_irq_save();
        for (u32 i = 0; i < LUME_MAX_PROCESSES; i++) {
            struct process *c = parent->children[i];

            if (!c || c->state == PROC_STATE_UNUSED)
                continue;
            if (pid > 0 && c->pid != (u32)pid)
                continue;
            if (pid == 0 && c->pgid != parent->pgid)
                continue;
            if (pid < -1 && c->pgid != (u32)(-pid))
                continue;

            if (c->state == PROC_STATE_ZOMBIE) {
                /* Reap: report the status, release the slot. */
                u32 status = (c->exit_code & 0xFF) << 8;

                if (status_out)
                    *status_out = status;
                ret = (int)c->pid;
                c->state = PROC_STATE_UNUSED;
                if (c->as)
                    vmm_space_destroy(c->as);
                parent->children[i] = NULL;
                break;
            }
            child = c;
        }
        arm_irq_restore(flags);

        if (ret >= 0)
            return ret;
        if (!child)
            return -ECHILD;      /* no matching child at all */
        if (options & WNOHANG)
            return 0;            /* children exist but none has exited */

        wait_event(&parent->waitq, 0);
    }
}

u32 proc_getuid(void)
{
    return current ? current->uid : 0;
}

u32 proc_geteuid(void)
{
    return current ? current->euid : 0;
}

u32 proc_getgid(void)
{
    return current ? current->gid : 0;
}

u32 proc_getegid(void)
{
    return current ? current->egid : 0;
}

void proc_dump(void (*emit)(const char *line, void *arg), void *arg)
{
    char line[160];

    for (u32 i = 0; i < LUME_MAX_PROCESSES; i++) {
        struct process *p = &processes[i];

        if (p->state == PROC_STATE_UNUSED)
            continue;
        ksnprintf(line, sizeof(line), "%5u %5u %5u %5u  %-8s %u/%u",
                  p->pid, p->ppid, p->pgid, p->sid,
                  p->state == PROC_STATE_ZOMBIE ? "zombie" : "alive",
                  p->used_fds, LUME_MAX_FDS);
        emit(line, arg);
    }
}
