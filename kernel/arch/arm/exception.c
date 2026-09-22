/*
 * LumeOS ARMv6 exception handling that is not a syscall or an IRQ:
 * data aborts, prefetch aborts and undefined instructions.
 *
 * Faults taken in user mode kill the process (there is no full demand-paging
 * or copy-on-write yet, so a user fault is a real bug in the program; the
 * kernel prints a diagnostic and terminates it).  Faults taken in kernel mode
 * are kernel bugs and panic.
 *
 * Fault status decoding uses DFSR/DFSR2 and IFSR (ARM1176 TRM chapter 3.4.4
 * "Fault status registers"); the bit assignments are the ARMv6 ones, which
 * differ from ARMv7 (in particular bit 12 is EXTWALK on ARMv6 and the "domain"
 * field is bits 7:4).
 */
#include <lume/asm.h>
#include <lume/irq.h>
#include <lume/errno.h>
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/panic.h>
#include <lume/proc.h>
#include <lume/sched.h>
#include <lume/string.h>
#include <lume/trapframe.h>
#include <lume/types.h>

/* Fault status codes (ARMv6, ARM1176 TRM 3.4.4). */
static const char *fault_status_string(u32 fsr, int data)
{
    u32 status = fsr & 0xF;
    u32 ext = (fsr >> 10) & 1;
    u32 domain = (fsr >> 4) & 0xF;

    static char buf[96];

    if (data) {
        switch (status) {
        case 0x1: return "alignment fault";
        case 0x4: return "external abort on translation (level 1)";
        case 0x5: return "translation fault (level 1 section)";
        case 0x6: return "access flag fault (level 1 section)";
        case 0x7: return "translation fault (level 2 page)";
        case 0x9: return "domain fault (level 1 section)";
        case 0xB: return "domain fault (level 2 page)";
        case 0xD: return "permission fault (level 1 section)";
        case 0xF: return "permission fault (level 2 page)";
        case 0x8: return "external abort (non-translation)";
        case 0xC: return "external abort on translation (level 2)";
        case 0xE: return "external abort (level 2 walk)";
        default:
            ksnprintf(buf, sizeof(buf), "unknown data fault (FSR=0x%08x, domain=%u, ext=%u)",
                      fsr, domain, ext);
            return buf;
        }
    }

    switch (status) {
    case 0x1: return "alignment fault";
    case 0x3: return "access flag fault (level 1 section)";
    case 0x5: return "translation fault (level 1 section)";
    case 0x6: return "access flag fault (level 2 page)";
    case 0x7: return "translation fault (level 2 page)";
    case 0x9: return "domain fault (level 1 section)";
    case 0xB: return "domain fault (level 2 page)";
    case 0xD: return "permission fault (level 1 section)";
    case 0xF: return "permission fault (level 2 page)";
    case 0x8: return "external abort (non-translation)";
    case 0xC: return "external abort on translation (level 1)";
    case 0xE: return "external abort (level 2 walk)";
    default:
        ksnprintf(buf, sizeof(buf), "unknown prefetch fault (FSR=0x%08x, domain=%u, ext=%u)",
                  fsr, domain, ext);
        return buf;
    }
}

static void dump_frame(const char *what, struct trapframe *tf, u32 fault_addr,
                       u32 fsr)
{
    pr_err("%s at pc=0x%08x (cpsr=0x%08x)", what, tf->pc, tf->cpsr);
    pr_err("  fault address 0x%08x, fsr=0x%08x (%s)", fault_addr, fsr,
           fault_status_string(fsr, strcmp(what, "prefetch abort") != 0));
    pr_err("  r0=%08x r1=%08x r2=%08x r3=%08x", tf->r[0], tf->r[1], tf->r[2], tf->r[3]);
    pr_err("  r4=%08x r5=%08x r6=%08x r7=%08x", tf->r[4], tf->r[5], tf->r[6], tf->r[7]);
    pr_err("  r8=%08x r9=%08x r10=%08x r11=%08x", tf->r[8], tf->r[9], tf->r[10], tf->r[11]);
    pr_err("  r12=%08x sp=%08x lr=%08x", tf->r[12], tf->sp, tf->lr);
}

/* A user-mode fault terminates the process.  Signals are not implemented far
 * enough yet to deliver SIGSEGV to a handler, so the process is killed with
 * the conventional exit status for a segmentation fault (128 + SIGSEGV); this
 * is reported honestly in docs/roadmap.md as a missing feature. */
static void kill_faulting_process(const char *why)
{
    struct process *p = proc_current();

    if (p && p->state == PROC_STATE_ALIVE) {
        pr_err("exception: killing pid %u: %s", p->pid, why);
        proc_exit(p, 128 + 11 /* SIGSEGV */);
    }
    panic("user fault with no user process: %s", why);
}

void do_dabt(struct trapframe *tf)
{
    u32 dfsr = arm_read_dfsr();
    u32 dfar = arm_read_dfar();

    dump_frame("data abort", tf, dfar, dfsr);

    if ((tf->cpsr & 0x1F) == MODE_USR) {
        struct process *p = proc_current();

        if (p)
            pr_err("  (user fault in pid %u)", p->pid);
        kill_faulting_process("data abort");
    }
    panic("kernel data abort at pc=0x%08x faulting address 0x%08x", tf->pc, dfar);
}

void do_pabt(struct trapframe *tf)
{
    u32 ifsr = arm_read_ifsr();
    u32 ifar = arm_read_ifar();

    dump_frame("prefetch abort", tf, ifar, ifsr);

    if ((tf->cpsr & 0x1F) == MODE_USR)
        kill_faulting_process("prefetch abort");

    panic("kernel prefetch abort at pc=0x%08x", tf->pc);
}

/*
 * Supervisor call entry.  The syscall layer is the next milestone
 * (docs/roadmap.md): there is no ELF loader and no userspace yet, so an SVC
 * cannot come from a program LumeOS loaded.  Instead of pretending to
 * dispatch, the handler reports the call exactly and kills the caller if it
 * came from user mode.  This is the hook the ARM Linux EABI dispatcher will
 * be installed on.
 */
void do_syscall(struct trapframe *tf)
{
    pr_err("syscall: SVC with r7=%u (0x%x) from %s mode, pc=0x%08x",
           tf->r[7], tf->r[7], (tf->cpsr & 0x1F) == MODE_USR ? "user" : "kernel",
           tf->pc);
    pr_err("syscall: the LumeOS syscall layer is not implemented yet "
           "(docs/roadmap.md)");

    if ((tf->cpsr & 0x1F) == MODE_USR)
        proc_exit(proc_current(), 128 + 31 /* SIGSYS */);

    panic("supervisor call from kernel mode at pc=0x%08x", tf->pc);
}

void do_undef(struct trapframe *tf)
{
    u32 instr = *(volatile u32 *)(tf->pc & ~3u);

    pr_err("undefined instruction 0x%08x at pc=0x%08x (cpsr=0x%08x)",
           instr, tf->pc, tf->cpsr);

    /* An undefined instruction from user mode could be a coprocessor access
     * that LumeOS does not emulate (a VFP instruction, for example: the
     * ARM1176JZF-S has no VFP).  Say so precisely instead of guessing. */
    if ((instr & 0x0E000000u) == 0x0C000000u)
        pr_err("  (this looks like a coprocessor/VFP instruction; the "
               "ARM1176JZF-S has no VFP, see docs/architecture.md)");

    dump_frame("undefined instruction", tf, tf->pc, 0);

    if ((tf->cpsr & 0x1F) == MODE_USR)
        kill_faulting_process("undefined instruction");

    panic("kernel undefined instruction at pc=0x%08x", tf->pc);
}
