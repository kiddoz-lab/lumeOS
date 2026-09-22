/*
 * LumeOS thread management.
 *
 * Threads are allocated from a static table (LUME_MAX_THREADS entries), each
 * with a 4 KiB kernel stack page from the physical allocator.  The kernel
 * stack is mapped through the linear kernel window, so it is visible in every
 * address space.
 */
#include <lume/asm.h>
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/panic.h>
#include <lume/proc.h>
#include <lume/sched.h>
#include <lume/string.h>
#include <lume/types.h>

struct thread *thread_current(void);

static struct thread *allocate_thread(const char *name)
{
    struct thread *t = NULL;
    u32 flags = arm_irq_save();
    u32 kstack_pa = 0;

    for (u32 i = 0; i < LUME_MAX_THREADS; i++) {
        t = thread_slot(i);
        if (t && t->state == THREAD_UNUSED)
            break;
        t = NULL;
    }
    arm_irq_restore(flags);
    if (!t)
        return NULL;

    kstack_pa = pmm_alloc_page();
    if (!kstack_pa)
        return NULL;

    memset(t, 0, sizeof(*t));
    t->kstack_pa = kstack_pa;
    t->kstack = (u8 *)PHYS_TO_VIRT(kstack_pa);
    t->tid = (u32)0;
    t->state = THREAD_READY;
    strncpy(t->name, name ? name : "thread", LUME_THREAD_NAME_LEN - 1);
    return t;
}

struct thread *thread_create(const char *name, void (*fn)(void *), void *arg)
{
    struct thread *t = allocate_thread(name);

    if (!t)
        return NULL;

    t->is_user = 0;
    t->kfn = fn;
    t->karg = arg;

    if (arch_thread_init_kernel(t, fn, arg) < 0) {
        pmm_free_page(t->kstack_pa);
        memset(t, 0, sizeof(*t));
        return NULL;
    }

    /* Give the thread an id and make it runnable. */
    u32 flags = arm_irq_save();
    t->tid = thread_assign_tid(t);
    sched_enqueue_thread(t);
    arm_irq_restore(flags);
    return t;
}

struct thread *thread_create_user(struct process *proc, const char *name,
                                  u32 entry, u32 user_sp, u32 arg)
{
    struct thread *t = allocate_thread(name);

    if (!t)
        return NULL;

    t->is_user = 1;
    t->proc = proc;

    if (arch_thread_init_user(t, entry, user_sp, arg) < 0) {
        pmm_free_page(t->kstack_pa);
        memset(t, 0, sizeof(*t));
        return NULL;
    }

    u32 flags = arm_irq_save();
    t->tid = thread_assign_tid(t);
    sched_enqueue_thread(t);
    arm_irq_restore(flags);
    return t;
}

void thread_exit(int code)
{
    struct thread *t = thread_current();

    arm_irq_disable();
    if (t) {
        if (t->is_user && t->proc)
            proc_note_thread_exit(t->proc, code);
        t->state = THREAD_DEAD;
        t->next = NULL;
    }
    /* Never returns: the scheduler frees the kernel stack of a dead thread
     * once it has switched away from it. */
    schedule();
    panic("thread_exit: dead thread was scheduled again");
}

void kthread_exit(void)
{
    thread_exit(0);
}
