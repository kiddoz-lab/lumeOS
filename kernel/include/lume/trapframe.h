/*
 * LumeOS trap frame layout.
 *
 * A trap frame is the saved register state of a thread that was interrupted
 * by an exception (syscall, interrupt or fault) *or* the initial register
 * state used to start a new thread.  It lives at the top of the thread's
 * kernel stack and is shared verbatim between the assembly entry code in
 * arch/arm/vectors.S and the C code in arch/arm/context.c; the offsets below
 * are the single source of truth for both.
 *
 * Frame layout (72 bytes, 8-byte aligned so that AAPCS calls from the
 * exception handlers stay aligned):
 *
 *   0x00 r0      0x04 r1      ...   0x30 r12
 *   0x34 sp (user stack pointer, or the interrupted kernel sp)
 *   0x38 lr (user link register)
 *   0x3C padding
 *   0x40 pc (the address execution resumes at)
 *   0x44 cpsr (the program status the thread had before the exception)
 *
 * The pc/cpsr pair is deliberately the last two words: the ARMv6 Store
 * Return State instruction (SRSDB, "exception processing enhancements"
 * introduced in ARMv6) writes lr_<mode>/spsr_<mode> straight into that pair,
 * which is how the IRQ path builds the frame in two instructions.
 */
#ifndef LUME_TRAPFRAME_H
#define LUME_TRAPFRAME_H

#define TF_R0   0x00
#define TF_R1   0x04
#define TF_R2   0x08
#define TF_R3   0x0C
#define TF_R4   0x10
#define TF_R5   0x14
#define TF_R6   0x18
#define TF_R7   0x1C
#define TF_R8   0x20
#define TF_R9   0x24
#define TF_R10  0x28
#define TF_R11  0x2C
#define TF_R12  0x30
#define TF_SP   0x34
#define TF_LR   0x38
#define TF_PAD  0x3C
#define TF_PC   0x40
#define TF_CPSR 0x44
#define TF_SIZE 0x48

/* Token that the generic entry macro expects to find at the low end of every
 * frame: the value of the "SVC / abort / undefined" banked lr register.  It
 * is only used by the assembly entry code. */

#ifndef __ASSEMBLER__
#ifndef __ASSEMBLY__

#include <lume/types.h>

struct trapframe {
    u32 r[13];   /* r0..r12 */
    u32 sp;      /* user stack pointer */
    u32 lr;      /* user link register */
    u32 pad;
    u32 pc;      /* resume address */
    u32 cpsr;    /* resume program status */
};

STATIC_ASSERT(sizeof(struct trapframe) == TF_SIZE, "trap frame layout");

/* The syscall number register for the ARM Linux EABI is r7. */
#define TF_SYSCALL_NUMBER_OFFSET TF_R7

#endif /* !__ASSEMBLY__ */
#endif /* !__ASSEMBLER__ */
#endif /* LUME_TRAPFRAME_H */
