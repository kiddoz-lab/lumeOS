/*
 * Exception round-trip probe.
 *
 * The trap frame is the contract between the assembly entry in
 * arch/arm/vectors.S and every C handler in the kernel, and until userspace
 * exists nothing exercised it: the kernel never executed an SVC, and the IRQ
 * path was only known to work by the fact that the system kept running.
 *
 * This probe makes that contract checkable.  arch/trapprobe.S fills r0..r12
 * with a known pattern, executes a supervisor call, and records what the
 * handler saw and what came back.  It walks the real path - vector_svc ->
 * exception_entry -> do_syscall -> __restore_regs - so a failure here is a
 * failure userspace would have hit, and two of the things it checks are the
 * ones a kernel stack cannot survive getting wrong:
 *
 *   - every register comes back unchanged, and
 *   - the stack pointer is exactly where it was before the call.
 *
 * Both were broken in the first version of the entry code (see the commit that
 * added the probe): an SVC from SVC mode relocated the frame onto the same
 * stack and never gave the space back, which leaks 64 bytes of kernel stack per
 * supervisor call - about fifty syscalls before a 4 KiB kernel stack runs into
 * whatever is below it.
 *
 * The probe's SVC number is the pattern for r7, so the frame's r7 slot is
 * checked like every other register and the handler can recognise the call
 * without a second magic number.
 *
 * The offsets below are shared with the assembly, the same way trapframe.h
 * shares the frame layout.
 */
#ifndef LUME_TRAPTEST_H
#define LUME_TRAPTEST_H

#define TRAPTEST_SVC_NUMBER  0xA5A50007u
#define TRAPTEST_REGS        13u

/* One macro, two spellings: the assembler has no casts, so the C version needs
 * the type and the assembly version must not see it. */
#ifdef __ASSEMBLER__
#define TRAPTEST_PATTERN(i)  (0xA5A50000 + (i))
#else
#define TRAPTEST_PATTERN(i)  (0xA5A50000u + (u32)(i))
#endif

#define TT_CALLS       0x00   /* SVCs the probe handler saw */
#define TT_RESUME      0x04   /* address after the probe's svc */
#define TT_SP_AT_SVC   0x08   /* sp when the probe trapped */
#define TT_SP_AFTER    0x0C   /* sp after the exception returned */
#define TT_CLOBBER     0x10   /* bit i set: register i did not survive */
#define TT_FINISHED    0x14   /* the probe returned to C */
#define TT_FRAME       0x18   /* 13 words: r0..r12 as the handler saw them */
#define TT_FRAME_SP    0x4C
#define TT_FRAME_LR    0x50
#define TT_FRAME_PC    0x54
#define TT_FRAME_CPSR  0x58
#define TT_AFTER       0x5C   /* 13 words: r0..r12 after the return */
#define TT_SIZE        0x90

#ifndef __ASSEMBLER__
#ifndef __ASSEMBLY__

#include <lume/trapframe.h>   /* includes lume/types.h as well */

struct traptest_report {
    u32 calls;
    u32 resume;
    u32 sp_at_svc;
    u32 sp_after;
    u32 clobber;
    u32 finished;
    u32 frame[TRAPTEST_REGS];
    u32 frame_sp;
    u32 frame_lr;
    u32 frame_pc;
    u32 frame_cpsr;
    u32 after[TRAPTEST_REGS];
};

STATIC_ASSERT(sizeof(struct traptest_report) == TT_SIZE, "trap probe report layout");

extern struct traptest_report traptest_report;

/* arch/arm/trapprobe.S: run one SVC round trip and fill in the report. */
void arch_trap_probe_run(void);

/* Called by do_syscall() when the probe's call arrives; ordinary syscalls must
 * never reach this. */
void traptest_capture(const struct trapframe *tf);

/* Zero the report so a stale result can never be mistaken for a fresh one. */
void traptest_reset(void);

#endif /* !__ASSEMBLY__ */
#endif /* !__ASSEMBLER__ */
#endif /* LUME_TRAPTEST_H */
