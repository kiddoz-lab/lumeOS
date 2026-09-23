/*
 * LumeOS scheduler.
 *
 * A uniprocessor round-robin scheduler with an explicit idle thread.
 *
 * Preemption model (see docs/architecture.md):
 *   - The kernel always runs with IRQ/FIQ masked.  Interrupts are only taken
 *     while userspace runs, or in the deliberate idle/blocked state entered
 *     from wait_event()/sched_idle().
 *   - The timer interrupt therefore preempts user mode directly.  On the way
 *     out of the exception, do_irq() checks sched_need_resched() and calls
 *     schedule(), which switches kernel stacks (and therefore user contexts).
 *   - Everything the scheduler touches runs with interrupts disabled, so no
 *     locking is required on this single-core target.
 */
#include <lume/asm.h>
#include <lume/klog.h>
#include <lume/panic.h>
#include <lume/mem.h>
#include <lume/proc.h>
#include <lume/sched.h>
#include <lume/string.h>
#include <lume/time.h>
#include <lume/types.h>

volatile u64 sched_context_switches;

static struct thread threads[LUME_MAX_THREADS];
static struct thread *ready_head;      /* round-robin ready queue */
static struct thread *ready_tail;
static struct thread *current;
static struct thread *idle_thread;
static volatile int need_resched;
static u32 next_tid = 1;

static void ready_enqueue(struct thread *t);
static void ready_remove(struct thread *t);

/* Sleeping threads with a deadline, kept in a simple list. */
static struct thread *sleepers;

struct thread *thread_slot(u32 index)
{
    return (index < LUME_MAX_THREADS) ? &threads[index] : NULL;
}

u32 thread_assign_tid(struct thread *t)
{
    return next_tid++;
}

void sched_enqueue_thread(struct thread *t)
{
    ready_enqueue(t);
}

/* Take a thread out of the ready queue.  Used when a thread is thrown away
 * before it ever ran (see thread_discard()); leaving it queued would let the
 * scheduler pick a thread whose stack has already been returned to the page
 * allocator. */
void sched_remove_thread(struct thread *t)
{
    if (t)
        ready_remove(t);
}

void thread_init_subsystem(void)
{
    memset(threads, 0, sizeof(threads));
    ready_head = ready_tail = NULL;
    current = NULL;
    need_resched = 0;
    next_tid = 1;
    sched_context_switches = 0;
}

struct thread *thread_by_tid(u32 tid)
{
    for (u32 i = 0; i < LUME_MAX_THREADS; i++)
        if (threads[i].state != THREAD_UNUSED && threads[i].tid == tid)
            return &threads[i];
    return NULL;
}

u32 thread_count(void)
{
    u32 n = 0;

    for (u32 i = 0; i < LUME_MAX_THREADS; i++)
        if (threads[i].state != THREAD_UNUSED)
            n++;
    return n;
}

struct thread *thread_current(void)
{
    return current;
}

static void ready_enqueue(struct thread *t)
{
    t->next = NULL;
    if (ready_tail)
        ready_tail->next = t;
    else
        ready_head = t;
    ready_tail = t;
    t->state = THREAD_READY;
}

static void ready_remove(struct thread *t)
{
    struct thread **link = &ready_head;

    while (*link) {
        if (*link == t) {
            *link = t->next;
            if (ready_tail == t)
                ready_tail = NULL; /* recomputed below when the list empties */
            break;
        }
        link = &(*link)->next;
    }
    if (!ready_head)
        ready_tail = NULL;
    else if (!ready_tail) {
        struct thread *last = ready_head;
        while (last->next)
            last = last->next;
        ready_tail = last;
    }
}

/* ------------------------------------------------------------------ */
/* Wait queues                                                         */
/* ------------------------------------------------------------------ */

void wait_queue_init(struct wait_queue *wq, const char *name)
{
    wq->head = NULL;
    wq->name = name;
}

void wait_queue_wake_one(struct wait_queue *wq)
{
    struct thread *t = wq->head;
    u32 flags = arm_irq_save();

    if (t) {
        wq->head = t->next;
        t->waitq = NULL;
        t->next = NULL;
        /* Remove from the sleeper list if it had a deadline. */
        if (t->wake_at_ms) {
            struct thread **link = &sleepers;
            while (*link) {
                if (*link == t) {
                    *link = t->next;
                    break;
                }
                link = &(*link)->next;
            }
            t->wake_at_ms = 0;
        }
        ready_enqueue(t);
    }
    arm_irq_restore(flags);
}

void wait_queue_wake_all(struct wait_queue *wq)
{
    while (wq->head)
        wait_queue_wake_one(wq);
}

u32 wait_queue_count(struct wait_queue *wq)
{
    u32 n = 0;
    struct thread *t;

    for (t = wq->head; t; t = t->next)
        n++;
    return n;
}

int wait_event(struct wait_queue *wq, u64 timeout_ms)
{
    struct thread *t = current;
    u32 flags = arm_irq_save();
    int ret = 0;

    if (!t) {
        arm_irq_restore(flags);
        return -1;
    }

    t->next = wq->head;
    wq->head = t;
    t->waitq = wq;
    t->state = THREAD_BLOCKED;

    if (timeout_ms) {
        t->wake_at_ms = time_monotonic_ms() + timeout_ms;
        t->next = sleepers;
        sleepers = t;
    }

    /* Hand the CPU to whoever is next; we resume here when woken. */
    schedule();

    flags = arm_irq_save();
    if (t->wake_at_ms && (time_monotonic_ms() >= t->wake_at_ms))
        ret = -1;
    t->wake_at_ms = 0;
    t->waitq = NULL;
    arm_irq_restore(flags);
    return ret;
}

void wait_wake_expired(u64 now_ms)
{
    struct thread *t = sleepers;

    while (t) {
        struct thread *next = t->next;

        if (t->wake_at_ms && now_ms >= t->wake_at_ms) {
            /* Unlink from the sleeper list and from its wait queue. */
            struct thread **link = &sleepers;
            while (*link) {
                if (*link == t) {
                    *link = t->next;
                    break;
                }
                link = &(*link)->next;
            }
            t->wake_at_ms = 0;
            if (t->waitq) {
                struct thread **wlink = &t->waitq->head;
                while (*wlink) {
                    if (*wlink == t) {
                        *wlink = t->next;
                        break;
                    }
                    wlink = &(*wlink)->next;
                }
                t->waitq = NULL;
            }
            if (t->state == THREAD_BLOCKED)
                ready_enqueue(t);
        }
        t = next;
    }
}

/* ------------------------------------------------------------------ */
/* Scheduler core                                                      */
/* ------------------------------------------------------------------ */

void sched_set_need_resched(void)
{
    need_resched = 1;
}

int sched_need_resched(void)
{
    return need_resched;
}

static void idle_loop(void *arg)
{
    for (;;) {
        /* Interrupts are enabled here on purpose: this is the only place a
         * kernel-mode IRQ can be taken, and the only thing that ever wakes
         * this thread. */
        arm_irq_enable();
        arm_wfi();
        arm_irq_disable();
        if (ready_head)
            schedule();
    }
}

void sched_init(void)
{
    /* The current execution context becomes thread 0 ("boot").  Its kernel
     * stack is the one boot.S installed, and the first context switch will
     * capture its stack pointer. */
    current = &threads[0];
    memset(current, 0, sizeof(*current));
    current->tid = 0;
    current->state = THREAD_RUNNING;
    current->name[0] = 'b';
    strncpy(current->name, "boot", LUME_THREAD_NAME_LEN);
    current->kstack = (u8 *)__builtin_frame_address(0);
    next_tid = 1;

    idle_thread = thread_create("idle", idle_loop, NULL);
    if (!idle_thread)
        panic("sched: cannot create the idle thread");
    pr_info("sched: running thread 0 (boot), idle thread tid %u", idle_thread->tid);
}

struct thread *sched_idle_thread(void)
{
    return idle_thread;
}

/*
 * Reaping a dead thread.
 *
 * thread_exit() cannot free its own kernel stack - it is standing on it - so a
 * thread that exits hands its stack to whoever runs next.  The stack is freed
 * at the top of the next schedule() call on a different stack, and the slot is
 * returned to the pool; a thread slot that never comes back would show up as
 * "fork stops working after 63 processes", which is the kind of bug that only
 * appears in the middle of a long-running program.
 */
static struct thread *reap_pending;

static void reap_thread(struct thread *t)
{
    if (!t || t->state != THREAD_DEAD)
        return;
    if (t->kstack_pa) {
        pmm_free_page(t->kstack_pa);
        t->kstack_pa = 0;
    }
    memset(t, 0, sizeof(*t));   /* state becomes THREAD_UNUSED (0) */
}

static struct thread *pick_next(void)
{
    struct thread *t;

    if (ready_head) {
        t = ready_head;
        ready_remove(t);
        return t;
    }
    return idle_thread;
}

void schedule(void)
{
    struct thread *prev = current;
    struct thread *next;
    u32 flags = arm_irq_save();

    need_resched = 0;

    /* Collect a thread that exited before this call.  Never the caller's own,
     * because its stack is the one we are using. */
    if (reap_pending && reap_pending != prev) {
        struct thread *dead = reap_pending;

        reap_pending = NULL;
        reap_thread(dead);
    }

    if (prev && prev->state == THREAD_RUNNING)
        prev->state = THREAD_READY;

    next = pick_next();
    if (!next)
        panic("sched: no runnable thread and no idle thread");

    if (next == prev) {
        if (prev->state == THREAD_READY)
            prev->state = THREAD_RUNNING;
        arm_irq_restore(flags);
        return;
    }

    if (prev && prev->state == THREAD_READY)
        ready_enqueue(prev);
    else if (prev && prev->state == THREAD_RUNNING)
        prev->state = THREAD_READY;

    next->state = THREAD_RUNNING;
    current = next;
    proc_switch_to(next);
    sched_context_switches++;

    if (prev && prev->state == THREAD_DEAD)
        reap_pending = prev;

    arch_switch_to(prev, next);

    /* When we get here, some other thread switched back to us.  `current` has
     * been restored by the scheduler of the thread that resumed us. */
    arm_irq_restore(flags);
}

void sched_yield(void)
{
    if (current)
        current->state = THREAD_READY;
    schedule();
}

void sched_tick(void)
{
    /* Called from the timer interrupt (IRQs masked by the exception entry). */
    need_resched = 1;
    wait_wake_expired(time_monotonic_ms());
}

void sched_sleep_ms(u64 ms)
{
    /* Sleep is implemented as a wait on a queue that nobody ever signals;
     * the timer wakes us through the deadline list. */
    static struct wait_queue sleep_queue;
    static int initialised;

    if (!initialised) {
        wait_queue_init(&sleep_queue, "sleep");
        initialised = 1;
    }
    wait_event(&sleep_queue, ms ? ms : 1);
}
