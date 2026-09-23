/*
 * Exception round-trip probe (see kernel/include/lume/traptest.h).
 *
 * void arch_trap_probe_run(void)
 *
 * Fills r0..r12 with the pattern defined in the header, executes an SVC whose
 * number is that same pattern's r7 slot, and records what the exception
 * handler saw and what came back.
 *
 * The SVC is not a convenience: it is the exact instruction a user program
 * will use to enter the kernel, and it takes the same entry path
 * (vector_svc -> exception_entry -> the C handler -> __restore_regs).  Two of
 * the checks it feeds - "every register came back unchanged" and "the stack
 * pointer is where it was" - are the two a kernel cannot get wrong and still
 * survive a program that makes a lot of syscalls.
 */
#include <lume/traptest.h>

	.text
	.arm
	.align	2
	.global	arch_trap_probe_run
arch_trap_probe_run:
	stmfd	sp!, {r4 - r11, lr}	@ preserve the AAPCS callee-saved set
	sub	sp, sp, #4		@ keep sp 8-byte aligned for the callee
	sub	sp, sp, #64		@ scratch: r0..r12 on the way back

	/* Where the stack is when the SVC traps: the frame has to record this
	 * value, and it has to come back to it. */
	ldr	r1, =traptest_report
	str	sp, [r1, #TT_SP_AT_SVC]

	/* Load the pattern through r14, not through r0..r12, so that no tested
	 * register has to hold the table address (and the probe does not depend
	 * on what an LDM does when its base register is in its own list). */
	adr	lr, .Lpattern
	ldmia	lr, {r0 - r12}
	svc	#0			@ r7 = TRAPTEST_SVC_NUMBER

.Lresume:
	/* The exception has returned.  Save what came back before touching any
	 * register: the base for this store is sp, so no scratch register is
	 * needed and all thirteen can be checked. */
	stmia	sp, {r0 - r12}

	ldr	r8, =traptest_report
	adr	r9, .Lpattern
	add	r7, r8, #TT_AFTER
	mov	r3, #0			@ bitmap of registers that did not survive
	mov	r4, #0			@ index
1:	ldr	r5, [sp, r4, lsl #2]
	ldr	r6, [r9, r4, lsl #2]
	cmp	r5, r6
	mov	r6, #1
	lsl	r6, r6, r4
	orrne	r3, r3, r6		@ flags still come from the cmp above
	str	r5, [r7, r4, lsl #2]
	add	r4, r4, #1
	cmp	r4, #TRAPTEST_REGS
	bne	1b

	str	r3, [r8, #TT_CLOBBER]
	adr	r1, .Lresume
	str	r1, [r8, #TT_RESUME]
	str	sp, [r8, #TT_SP_AFTER]
	mov	r1, #1
	str	r1, [r8, #TT_FINISHED]

	add	sp, sp, #68
	ldmfd	sp!, {r4 - r11, pc}

	.align	2
.Lpattern:
	.word	TRAPTEST_PATTERN(0)
	.word	TRAPTEST_PATTERN(1)
	.word	TRAPTEST_PATTERN(2)
	.word	TRAPTEST_PATTERN(3)
	.word	TRAPTEST_PATTERN(4)
	.word	TRAPTEST_PATTERN(5)
	.word	TRAPTEST_PATTERN(6)
	.word	TRAPTEST_PATTERN(7)
	.word	TRAPTEST_PATTERN(8)
	.word	TRAPTEST_PATTERN(9)
	.word	TRAPTEST_PATTERN(10)
	.word	TRAPTEST_PATTERN(11)
	.word	TRAPTEST_PATTERN(12)
