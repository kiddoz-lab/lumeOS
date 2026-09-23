/*
 * LumeOS ARMv6 exception entry and exit.
 *
 * The architectural vector table lives on the vector page at virtual
 * 0xFFFF0000 (copied there by boot.S while the MMU is still off).  Each entry
 * is "ldr pc, [pc, #0x18]", so it jumps straight to one of the *_entry stubs
 * below through the literal pool that follows the eight vector words.
 *
 * Design notes (see docs/architecture.md for the full story):
 *
 *  - Interrupts are masked whenever the kernel runs, except in the deliberate
 *    wait points of the scheduler.  A trap therefore either comes from user
 *    mode or from one of those kernel wait points.
 *
 *  - Entry happens in two stages.  First the frame is built on the trapping
 *    mode's own stack (sp_irq/sp_abt/sp_und, set up by boot.S).  Then control
 *    switches to SVC mode, where the frame is copied onto the current thread's
 *    kernel stack (sp_svc) and the C handler runs.  Doing it this way means
 *    the entry code never needs a spare register to hold a stack pointer: the
 *    frame slots are addressed as constant offsets from the trapping sp.
 *
 *  - The trapped context's r12 is stored straight into the frame's r12 slot
 *    before r12 is reused as a scratch register, so no scratch word and no
 *    clobbering of any trapped register is needed.
 *
 *  - User-bank sp/lr are captured with "stmdb r12, {sp, lr}^", which stores
 *    the *user* bank registers even while running in a privileged mode.
 */

#include <lume/trapframe.h>

/* These mirror lume/asm.h, which cannot be included here (it is C).  The
 * in-kernel self test compares a real trap frame's mode against the C
 * constants, so a drift between the two lists fails a check instead of
 * silently changing which register bank an exception returns to. */
#define MODE_USR 0x10
#define MODE_IRQ 0x12
#define MODE_SVC 0x13
#define MODE_SYS 0x1F
#define MODE_MASK 0x1F

/* ------------------------------------------------------------------ */
/* Generic entry: builds a trap frame, switches to SVC mode and calls  */
/* the C handler.                                                      */
/*                                                                     */
/* \handler is called with r0 = pointer to the frame.  On return the   */
/* frame is popped by __restore_regs.                                  */
/*                                                                     */
/* \in_place is 1 for the SVC vector only.  Its trap mode *is* SVC     */
/* mode, so the frame is already being built on the stack the handler  */
/* is going to use; relocating it (which the other vectors must do,    */
/* because they trap onto a different bank's stack) would leave the    */
/* kernel stack pointer 64 bytes lower after every supervisor call -   */
/* a leak of about fifty syscalls before a 4 KiB kernel stack reaches  */
/* whatever is below it.                                               */
/* ------------------------------------------------------------------ */
	.macro	exception_entry, name, handler, correction=0, in_place=0
	.global	\name
	.align	5
\name:
	.if \correction
	sub	lr, lr, #\correction	@ point lr at the interrupted instruction
	.endif

	/* Reserve the top part of the frame on the trapping mode's stack.
	 * After this, sp = frame base + 8, and every slot is a constant
	 * offset from sp. */
	sub	sp, sp, #(TF_SIZE - 8)

	stmdb	sp, {r0, r1}			@ frame.r0, frame.r1
	str	r12, [sp, #(TF_R12 - 8)]	@ frame.r12 (r12 itself is now free)
	mrs	r12, spsr
	str	r12, [sp, #(TF_CPSR - 8)]	@ frame.cpsr
	and	r12, r12, #MODE_MASK

	/* r13 and r14 are banked per mode, so what they mean depends on where
	 * the trap came from.  A trap from user mode - or from System mode,
	 * which shares the user bank - captures the user bank with the ^ form.
	 * Any other trap stores this mode's own sp/lr; the second half of the
	 * entry replaces the sp with the interrupted kernel stack pointer once
	 * it is back in SVC mode and can actually read it. */
	cmp	r12, #MODE_USR
	beq	7f
	cmp	r12, #MODE_SYS
	bne	8f
7:	add	r12, sp, #TF_SP			@ stmdb base: sp lands at TF_SP
	stmdb	r12, {sp, lr}^			@ user-bank sp/lr
	b	9f
8:	add	r12, sp, #TF_SP
	stmdb	r12, {sp, lr}			@ this mode's own sp/lr
9:	stmia	sp, {r2 - r11}			@ frame.r2 .. frame.r11
	str	lr, [sp, #(TF_PC - 8)]		@ frame.pc (the resume address)

	sub	r0, sp, #8			@ r0 = frame base
	mov	r1, #0
	str	r1, [r0, #TF_PAD]

	/* Switch to SVC mode without touching r0, with interrupts masked.
	 * The kernel runs with IRQs and FIQs disabled except at the explicit
	 * wait points in the scheduler, and a trap can arrive from a context
	 * that had them enabled - user mode will. */
	mrs	r1, cpsr
	bic	r1, r1, #0x3F
	orr	r1, r1, #(MODE_SVC | 0xC0)
	msr	spsr_cxsf, r1
	adr	lr, 1f
	movs	pc, lr
1:
	.if \in_place
	/* The frame is already where the handler needs it (this stack), so
	 * expose it rather than moving it. */
	sub	sp, sp, #8			@ sp = frame base
	.else
	/* Relocate the frame onto the current kernel stack (sp_svc) and hand
	 * it to C. */
	sub	sp, sp, #TF_SIZE
	ldmia	r0, {r1 - r12}
	stmia	sp, {r1 - r12}
	add	r2, sp, #48
	add	r0, r0, #48
	ldmia	r0, {r3 - r8}
	stmia	r2, {r3 - r8}
	.endif

	/* The frame records the interrupted stack pointer, which is only
	 * knowable here: for a trap from a privileged mode it is this stack
	 * pointer plus the space the entry reserved, because the frame sits at
	 * the top of exactly that stack.  A user-bank capture is already the
	 * right answer and must be left alone.  See lume/trapframe.h. */
	ldr	r1, [sp, #TF_CPSR]
	and	r1, r1, #MODE_MASK
	cmp	r1, #MODE_USR
	beq	2f
	cmp	r1, #MODE_SYS
	beq	2f
	add	r1, sp, #TF_SIZE
	str	r1, [sp, #TF_SP]
2:
	mov	r0, sp
	bl	\handler
	b	__restore_regs
	.endm

	.text
	.arm

/* ------------------------------------------------------------------ */
/* Exception entries                                                   */
/* ------------------------------------------------------------------ */

	/* Supervisor call: r7 holds the syscall number (ARM EABI), lr points
	 * at the instruction after the SVC, so no correction is needed.  This is
	 * the one vector whose trap mode is SVC mode, hence in_place=1. */
	exception_entry vector_svc, do_syscall, 0, 1

	/* IRQ: lr points one instruction past the interrupted one. */
	exception_entry vector_irq, do_irq, 4

	/* Data abort: subtract 8 to get the aborted load/store. */
	exception_entry vector_dabt, do_dabt, 8

	/* Prefetch abort: subtract 4 to get the aborted fetch. */
	exception_entry vector_pabt, do_pabt, 4

	/* Undefined instruction: subtract 4 to get the faulting encoding. */
	exception_entry vector_und, do_undef, 4

/* ------------------------------------------------------------------ */
/* Exceptions LumeOS does not use: reset, reserved and FIQ.            */
/* ------------------------------------------------------------------ */
	.global	vector_reset
vector_reset:
	ldr	r0, =str_reset
	b	panic_asm

	.global	vector_reserved
vector_reserved:
	ldr	r0, =str_reserved
	b	panic_asm

	.global	vector_fiq
vector_fiq:
	ldr	r0, =str_fiq
	b	panic_asm

/* ------------------------------------------------------------------ */
/* Return to the trapped context                                       */
/* ------------------------------------------------------------------ */
/* sp points at a trap frame.  "User" below means *the register bank the trap
 * came from*: USR and SYS share the user bank for r13/r14, so both are
 * restored with the ^ form.  Everything else is a privileged context, whose
 * registers are restored in place.
 *
 * Both paths write SPSR and return with "movs pc, lr".  That is not style:
 * writing CPSR and then popping the frame would let an interrupt in between
 * re-enter the entry code on this stack while the frame is half popped, and
 * it would also re-enable interrupts before the return address is known to be
 * sound.  The status change and the branch are one instruction.
 *
 * The privileged path deliberately restores r0-r12 only: r13 comes back from
 * the frame pop (the frame sits at the top of this stack) and r14 holds the
 * resume address loaded above, so neither may be in the list.
 */
	.global	__restore_regs
__restore_regs:
	ldr	r1, [sp, #TF_CPSR]
	ldr	lr, [sp, #TF_PC]
	and	r2, r1, #MODE_MASK
	cmp	r2, #MODE_USR
	beq	1f
	cmp	r2, #MODE_SYS
	beq	1f

	/* privileged */
	msr	spsr_cxsf, r1
	ldm	sp, {r0 - r12}
	add	sp, sp, #TF_SIZE
	movs	pc, lr

1:	/* user bank: r0-r12 land in the current registers, r13/r14 go straight
	 * into the user bank instead of overwriting the kernel stack pointer */
	msr	spsr_cxsf, r1
	ldm	sp, {r0 - lr}^
	add	sp, sp, #TF_SIZE
	movs	pc, lr

/* ------------------------------------------------------------------ */
/* Kernel thread context switch                                        */
/* ------------------------------------------------------------------ */
/* void __switch_to(struct thread *prev, struct thread *next);
 *
 * Saves the callee-saved kernel registers on the current kernel stack,
 * stores the resulting stack pointer in prev->ksp (the first member of
 * struct thread) and resumes next->ksp.  A thread's user register state
 * lives in a trap frame further down its own kernel stack, so switching
 * stacks is all that is required. */
	.global	__switch_to
__switch_to:
	stmfd	sp!, {r4 - r11, lr}
	str	sp, [r0, #0]
	ldr	sp, [r1, #0]
	ldmfd	sp!, {r4 - r11, pc}

/* ------------------------------------------------------------------ */
/* Kernel thread trampoline                                            */
/* ------------------------------------------------------------------ */
/* A new kernel thread resumes here with r4 = entry function and
 * r5 = argument (set up by arch_thread_init_kernel). */
	.global	__kernel_thread_entry
__kernel_thread_entry:
	mov	r0, r5
	blx	r4
	bl	kthread_exit
	b	.

/* ------------------------------------------------------------------ */
/* Early panic helper: usable before the C runtime is ready            */
/* ------------------------------------------------------------------ */
	.global	panic_asm
panic_asm:
	cpsid	if
	mov	r1, lr
	bl	panic_early
	b	.

	.section .rodata
str_reset:
	.asciz	"reset exception taken"
str_reserved:
	.asciz	"reserved exception taken"
str_fiq:
	.asciz	"FIQ taken, but LumeOS never enables FIQ"

/* ------------------------------------------------------------------ */
/* Per-mode stacks (.bss, zeroed by the boot stub)                     */
/*                                                                     */
/* IRQ/ABT/UND/FIQ entry only needs room for one trap frame (72 bytes) */
/* plus the C handler's own frames, but 4 KiB keeps the mapping simple */
/* and lets abort handlers print diagnostics without worrying about    */
/* the stack.  The SVC stack here is the boot thread's kernel stack.   */
/* ------------------------------------------------------------------ */
#define TRAP_STACK_SIZE 4096
	.bss
	.align	8

/* Stacks grow downwards, so each region is defined bottom-first and the
 * exported *_top symbol sits immediately after the reserved space.  (Getting
 * this backwards made every stack overlap the next one; caught by the QEMU
 * boot test's address-to-symbol diagnosis.) */
	.global	__stack_irq_bottom
__stack_irq_bottom:
	.space	TRAP_STACK_SIZE
	.global	__stack_irq_top
__stack_irq_top:

	.align	8
	.global	__stack_fiq_bottom
__stack_fiq_bottom:
	.space	TRAP_STACK_SIZE
	.global	__stack_fiq_top
__stack_fiq_top:

	.align	8
	.global	__stack_abt_bottom
__stack_abt_bottom:
	.space	TRAP_STACK_SIZE
	.global	__stack_abt_top
__stack_abt_top:

	.align	8
	.global	__stack_und_bottom
__stack_und_bottom:
	.space	TRAP_STACK_SIZE
	.global	__stack_und_top
__stack_und_top:

	.align	8
	.global	__stack_svc_bottom
__stack_svc_bottom:
	.space	TRAP_STACK_SIZE * 2
	.global	__stack_svc_top
__stack_svc_top:
	.align	8
