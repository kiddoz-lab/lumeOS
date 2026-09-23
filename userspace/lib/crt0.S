/*
 * LumeOS userspace startup (ARM EABI).
 *
 * This is a freestanding program: there is no libc, no dynamic linker and no
 * relocation processing, because the LumeOS kernel loads a static ET_EXEC
 * image at the address it was linked for.  What is left of "starting a C
 * program" is what the Linux ABI guarantees at process entry:
 *
 *      sp -> [ argc ][ argv[0] ... argv[argc-1] ][ NULL ][ envp ... ][ NULL ]
 *
 * so _start reads argc from the stack, computes argv, calls main and passes
 * main's return value to exit(2).  r0/r1 also carry argc/argv, which is what
 * glibc's own entry code uses; both are set up by the kernel.
 */
#include <lume/syscall.h>

	.text
	.arm
	.align	2
	.global	_start
	.type	_start, %function
_start:
	mov	r0, sp			@ r0 = &argc
	ldr	r1, [r0]		@ argc
	add	r2, r0, #4		@ argv
	mov	r0, r1			@ main(argc, argv)
	mov	r1, r2
	bl	main

	/* Fall through into exit(main's return value).  r0 already holds it. */
	mov	r7, #LUME_SYS_exit
	svc	#0

	/* exit(2) must not return; if it ever does, retry once with exit_group
	 * and then stop rather than executing whatever follows in .text. */
	mov	r7, #LUME_SYS_exit_group
	svc	#0
1:	b	1b

/*
 * A handful of freestanding string functions.  The compiler is free to turn a
 * struct copy or a loop into a call to memcpy/memset, and with -nostdlib
 * nothing else would provide them.  Keeping them here (rather than pulling in
 * a libc) is what keeps the image small enough to embed in the kernel during
 * bring-up.
 */
	.global	memcpy
	.type	memcpy, %function
memcpy:
	mov	r12, r0
1:	cmp	r2, #0
	beq	2f
	ldrb	r3, [r1], #1
	strb	r3, [r0], #1
	sub	r2, r2, #1
	b	1b
2:	mov	r0, r12
	bx	lr

	.global	memset
	.type	memset, %function
memset:
	mov	r12, r0
1:	cmp	r2, #0
	beq	2f
	strb	r1, [r0], #1
	sub	r2, r2, #1
	b	1b
2:	mov	r0, r12
	bx	lr

	.global	memmove
	.type	memmove, %function
memmove:
	mov	r12, r0
	cmp	r0, r1
	bls	memcpy			@ copying down: the memcpy loop is safe
	add	r1, r1, r2		@ copying up: start from the end
	add	r0, r0, r2
1:	cmp	r2, #0
	beq	2f
	ldrb	r3, [r1, #-1]!
	strb	r3, [r0, #-1]!
	sub	r2, r2, #1
	b	1b
2:	mov	r0, r12
	bx	lr
