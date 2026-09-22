/*
 * LumeOS ARMv6 thread context management.
 *
 * A thread's kernel stack is a single 4 KiB page.  At its top sits either a
 * trap frame (user threads: the register state to restore on the way back to
 * user mode) or a plain kernel context block (kernel threads: the
 * callee-saved registers a context switch restores).
 *
 * Both cases put a 9-word block immediately below the trap frame:
 *
 *      [ r4 ][ r5 ][ r6 ][ r7 ][ r8 ][ r9 ][ r10 ][ r11 ][ pc ]
 *                                                        ^-- the address
 *      __switch_to does "ldmfd sp!, {r4-r11, pc}"             execution
 *                                                            resumes at
 *
 * so the block's address is exactly what struct thread::ksp must hold.  For a
 * user thread the "pc" is __restore_regs, which then pops the trap frame and
 * returns to user mode; for a kernel thread it is __kernel_thread_entry with
 * r4 = function and r5 = argument.
 */
#include <lume/asm.h>
#include <lume/config.h>
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/panic.h>
#include <lume/proc.h>
#include <lume/sched.h>
#include <lume/string.h>
#include <lume/trapframe.h>
#include <lume/types.h>

/* Provided by vectors.S */
extern void __switch_to(struct thread *prev, struct thread *next);
extern void __kernel_thread_entry(void);
extern void __restore_regs(void);

#define KSTACK_TOP(t) ((u32)(t)->kstack + LUME_KERNEL_STACK_SIZE)

static u32 *context_block_for(struct trapframe *tf)
{
    /* Nine words below the trap frame, 8-byte aligned. */
    return (u32 *)tf - 9;
}

int arch_thread_init_kernel(struct thread *t, void (*fn)(void *), void *arg)
{
    u32 top = KSTACK_TOP(t);
    struct trapframe *tf = (struct trapframe *)(top - TF_SIZE);
    u32 *ctx = context_block_for(tf);

    memset(tf, 0, TF_SIZE);
    memset(ctx, 0, 9 * sizeof(u32));

    ctx[0] = (u32)fn;                     /* r4 */
    ctx[1] = (u32)arg;                    /* r5 */
    ctx[8] = (u32)(uintptr_t_lume)__kernel_thread_entry; /* pc */

    t->ksp = (u32)ctx;
    t->tf = NULL;
    return 0;
}

int arch_thread_init_user(struct thread *t, u32 entry, u32 user_sp, u32 arg)
{
    u32 top = KSTACK_TOP(t);
    struct trapframe *tf = (struct trapframe *)(top - TF_SIZE);
    u32 *ctx = context_block_for(tf);

    memset(ctx, 0, 9 * sizeof(u32));
    ctx[8] = (u32)(uintptr_t_lume)__restore_regs;

    memset(tf, 0, TF_SIZE);
    tf->r[0] = arg;
    tf->sp = user_sp;
    tf->lr = 0;
    tf->pc = entry;
    /* User mode with IRQs and FIQs enabled (CPSR.I = CPSR.F = 0).  FIQ is only
     * ever enabled if the interrupt controller is configured for it, which
     * LumeOS never does. */
    tf->cpsr = MODE_USR;

    t->ksp = (u32)ctx;
    t->tf = tf;
    return 0;
}

void arch_switch_to(struct thread *prev, struct thread *next)
{
    struct vm_space *next_as;

    /* Kernel threads always run in the kernel address space; user threads run
     * in their process address space. */
    if (next->is_user && next->proc)
        next_as = next->proc->as;
    else
        next_as = vmm_kernel_space();

    if (next_as && next_as != vmm_current_space())
        vmm_switch_to(next_as);

    /* The TLS pointer (TPIDRURO, ARMv6K) is per-thread but not part of the
     * banked register set, so it must be reprogrammed on every switch. */
    if (next->is_user) {
        u32 tls = (next->proc && next->proc->tls) ? next->proc->tls : 0;

        arm_write_tpidruro(tls);
    }

    __switch_to(prev, next);
}

/** Return the trap frame of the running thread (used by the syscall layer to
 *  validate that the caller really is a user thread). */
struct trapframe *arch_current_trapframe(void)
{
    struct thread *t = thread_current();

    return t ? t->tf : NULL;
}
