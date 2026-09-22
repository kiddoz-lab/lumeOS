/*
 * LumeOS threads and scheduling.
 *
 * Model: a thread is a kernel-scheduled execution context with either a user
 * trap frame (user thread) or a plain kernel function (kernel thread).  The
 * scheduler is a simple round-robin over a ready queue; the timer interrupt
 * only ever preempts *user* mode (the kernel masks IRQs and calls
 * schedule() at explicit points), which keeps the kernel preemption model
 * trivially safe on a uniprocessor.
 */
#ifndef LUME_SCHED_H
#define LUME_SCHED_H

#include <lume/trapframe.h>
#include <lume/types.h>

#define LUME_MAX_THREADS 64
#define LUME_THREAD_NAME_LEN 16

enum thread_state {
    THREAD_UNUSED = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_DEAD,
};

struct process;
struct wait_queue;

struct thread {
    u32 ksp;                    /* MUST be the first member: vectors.S uses offset 0 */
    u32 tid;
    int state;
    int is_user;
    struct process *proc;
    struct trapframe *tf;       /* user register state (NULL for kernel threads) */
    u32 kstack_pa;              /* physical address of the kernel stack page */
    u8 *kstack;                 /* kernel stack virtual base */
    void (*kfn)(void *);        /* kernel thread entry point */
    void *karg;
    u64 wake_at_ms;             /* absolute time for timed sleeps (0 = none) */
    struct wait_queue *waitq;
    struct thread *next;        /* ready queue / wait queue link */
    char name[LUME_THREAD_NAME_LEN];
};

/* ------------------------------------------------------------------ */
/* Threads                                                             */
/* ------------------------------------------------------------------ */

void thread_init_subsystem(void);
struct thread *thread_slot(u32 index);
u32  thread_assign_tid(struct thread *t);
void sched_enqueue_thread(struct thread *t);
struct thread *thread_create(const char *name, void (*fn)(void *), void *arg);
struct thread *thread_create_user(struct process *proc, const char *name,
                                  u32 entry, u32 user_sp, u32 arg);
void thread_exit(int code) __noreturn;
void kthread_exit(void) __noreturn;

struct thread *thread_current(void);
struct thread *thread_by_tid(u32 tid);
u32 thread_count(void);

/* Architecture hooks implemented in arch/arm/context.c. */
int  arch_thread_init_kernel(struct thread *t, void (*fn)(void *), void *arg);
int  arch_thread_init_user(struct thread *t, u32 entry, u32 user_sp, u32 arg);
void arch_switch_to(struct thread *prev, struct thread *next);

/* ------------------------------------------------------------------ */
/* Wait queues                                                         */
/* ------------------------------------------------------------------ */
struct wait_queue {
    struct thread *head;
    const char *name;
};

void wait_queue_init(struct wait_queue *wq, const char *name);
void wait_queue_wake_one(struct wait_queue *wq);
void wait_queue_wake_all(struct wait_queue *wq);
u32  wait_queue_count(struct wait_queue *wq);

/** Block the current thread on the queue.  timeout_ms == 0 means "forever".
 *  Returns 0 when woken, -1 on timeout. */
int wait_event(struct wait_queue *wq, u64 timeout_ms);

/** Wake every thread sleeping with a deadline in the past (called from the
 *  timer interrupt). */
void wait_wake_expired(u64 now_ms);

/* ------------------------------------------------------------------ */
/* Scheduler                                                           */
/* ------------------------------------------------------------------ */

void sched_init(void);
void schedule(void);
void sched_yield(void);
void sched_tick(void);
void sched_set_need_resched(void);
int  sched_need_resched(void);
void sched_sleep_ms(u64 ms);
struct thread *sched_idle_thread(void);

extern volatile u64 sched_context_switches;

#endif /* LUME_SCHED_H */
