/*
 * LumeOS processes.
 *
 * A process is the Linux-compatible container: address space, file descriptor
 * table, working directory, TLS pointer, credential ids, exit status and the
 * parent/child relationship a shell needs in order to wait().
 *
 * Threads belong to processes.  LumeOS currently creates exactly one thread
 * per process (clone() without CLONE_THREAD behaves as fork), but the
 * structures already separate the two so a future clone(CLONE_VM|CLONE_THREAD)
 * implementation does not need a redesign.
 */
#ifndef LUME_PROC_H
#define LUME_PROC_H

#include <lume/compiler.h>
#include <lume/fs.h>
#include <lume/sched.h>
#include <lume/types.h>

#define LUME_MAX_PROCESSES 32
#define LUME_MAX_FDS 64

#define PROC_STATE_UNUSED  0
#define PROC_STATE_ALIVE   1
#define PROC_STATE_ZOMBIE  2

enum {
    PROC_IDLE = 0,   /* the kernel/boot context, pid 0 */
    PROC_USER,
};

struct process {
    u32 pid;
    u32 ppid;
    u32 pgid;
    u32 sid;
    int state;
    int kind;

    struct vm_space *as;
    u32 brk;          /* current program break (Linux brk(2)) */
    u32 brk_base;     /* what the loader left behind: the heap's floor */
    struct file *fds[LUME_MAX_FDS];
    char cwd[LUME_PATH_MAX];

    u32 tls;              /* TPIDRURO value for this process */
    int exit_code;        /* low 8 bits: WEXITSTATUS */
    int killed_by_signal;
    u32 uid, gid, euid, egid;

    struct process *parent;
    struct process *children[LUME_MAX_PROCESSES];
    struct process *sibling_next;

    struct wait_queue waitq;   /* a parent blocks here */
    struct wait_queue sigq;    /* threads block here waiting for a child */

    u32 threads;               /* number of live threads */
    struct thread *thread;     /* the primary thread (one thread per process today) */
    int used_fds;
    int is_init;
};

void proc_init(void);
struct process *proc_current(void);
struct process *proc_by_pid(u32 pid);
u32 proc_alloc_pid(void);

/** Build the process struct plus its primary thread, ready to be scheduled.
 *  `entry` and `user_sp` describe the user mode resume point. */
struct process *proc_create(const char *name, u32 entry, u32 user_sp, u32 arg,
                            struct process *parent);

void proc_set_current(struct process *p);

/** The kernel's own process (pid 0): what a kernel thread runs as. */
struct process *proc_kernel_process(void);

/** Called by the scheduler on every switch: a user thread runs as its process,
 *  a kernel thread runs as pid 0.  Without this the syscall layer would
 *  attribute a user program's syscalls to whatever ran before it. */
void proc_switch_to(struct thread *t);

/** Record where the loaded image ended, so brk(2) knows where the heap may
 *  start and how far down it may be given back. */
void proc_set_brk_base(struct process *p, u32 base);
void proc_note_thread_exit(struct process *p, int code);

/** Wait for a child to change state.  Linux wait4(pid, status, options,
 *  rusage) semantics with options = WNOHANG supported. */
int proc_wait(struct process *parent, s32 pid, u32 options, u32 *status_out);

/** Undo a process whose creation did not complete (nothing to reap, nothing
 *  left behind).  Not for a running process: that is proc_exit(). */
void proc_discard(struct process *p, struct process *parent);

/** Deliver an exit status to the parent and mark the process as a zombie. */
void proc_exit(struct process *p, int code) __noreturn;

void proc_dump(void (*emit)(const char *line, void *arg), void *arg);

/* wait4(2) options and status decoding (Linux ARM values). */
#define WNOHANG    1
#define WUNTRACED  2
#define WSTOPPED   2
#define WEXITED    4
#define WCONTINUED 8
#define WNOWAIT    0x01000000

#define WEXITSTATUS(s) (((s) & 0xFF00) >> 8)
#define WTERMSIG(s)    ((s) & 0x7F)
#define WIFEXITED(s)   (WTERMSIG(s) == 0)
#define WIFSIGNALED(s) (((signed char)(((s) & 0x7F) + 1) >> 1) > 0)

/* Credential helpers (Linux ABI: uids/gids are 32-bit in syscalls). */
u32 proc_getuid(void);
u32 proc_geteuid(void);
u32 proc_getgid(void);
u32 proc_getegid(void);

#endif /* LUME_PROC_H */
