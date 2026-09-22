/*
 * LumeOS ARMv6 boot stub.
 *
 * The Raspberry Pi boot firmware loads a flat binary at physical 0x8000 and
 * jumps to its first byte (see docs/boot-pi.md for the exact chain and the
 * references).  LumeOS is linked to run from the kernel virtual alias
 * 0xC0000000 + physical, so this stub runs with the MMU off, builds the
 * initial translation table, enables the MMU/caches and branches to the high
 * alias.  It is position independent and uses no stack.
 *
 * Physical memory reserved by/for this stub:
 *   0x00004000 - 0x00007FFF  level 1 translation table (16 KiB)
 *   0x00008000 - ...         the kernel image
 *   0x000F0000 - 0x000F0FFF  exception vector page (mapped at 0xFFFF0000)
 */

#include <lume/pte.h>

#define L1_TABLE_PA        0x00004000
#define VECTOR_PAGE_PA     0x000F0000
#define VECTOR_PAGE_SIZE   0x1000

#define SCTLR_M   (1 << 0)
#define SCTLR_A   (1 << 1)
#define SCTLR_C   (1 << 2)
#define SCTLR_I   (1 << 12)
#define SCTLR_V   (1 << 13)
#define SCTLR_Z   (1 << 11)
#define SCTLR_U   (1 << 22)

	.section .text.boot, "ax", %progbits
	.arm
	.align 4
	.global _start
	.type _start, %function
_start:
	/* Entry state on Raspberry Pi with 32-bit firmware:
	 *   r0 = 0
	 *   r1 = machine type (MACH_TYPE_BCM2708) or 0 for device-tree boots
	 *   r2 = physical address of the ATAGS list or device tree blob
	 * The registers are preserved here for the C startup code. */
	cps	#0x13			@ SVC mode, supervisor
	cpsid	if			@ interrupts off while we restructure the world

	mov	r9, r2			@ firmware-provided fdt/atags pointer
	adr	r10, _start		@ where we were really loaded (diagnostics)

	/* Invalidate I-cache, D-cache, branch predictor and TLB so that no
	 * stale state from the firmware survives into LumeOS. */
	mov	r0, #0
	mcr	p15, 0, r0, c7, c5, 0	@ ICIALLU      : invalidate I-cache
	mcr	p15, 0, r0, c7, c6, 0	@ DCIMVAC all : invalidate D-cache
	mcr	p15, 0, r0, c7, c5, 6	@ BPIALL       : invalidate branch predictor
	mcr	p15, 0, r0, c8, c7, 0	@ TLBIALL      : invalidate TLB
	mcr	p15, 0, r0, c7, c10, 4	@ DSB
	mcr	p15, 0, r0, c7, c5, 4	@ ISB

	/* Copy the exception vector table (64 bytes) to the vector page. */
	adr	r4, _vector_template
	ldr	r5, =VECTOR_PAGE_PA
	mov	r6, #64
1:	ldmia	r4!, {r0, r1}
	stmia	r5!, {r0, r1}
	subs	r6, r6, #8
	bne	1b

	/* Zero the level 1 table (4096 entries). */
	ldr	r4, =L1_TABLE_PA
	mov	r5, #0
	mov	r6, #4096
1:	str	r5, [r4], #4
	subs	r6, r6, #1
	bne	1b

	/* Level 1 table layout (all in domain 0, all supervisor-accessible):
	 *
	 *   [0x000]        1 MiB section -> physical 0x00000000, cached.
	 *                  Temporary identity mapping used by the trampoline
	 *                  below; cleared once we run at the high alias.
	 *   [0xC00..0xCFF] 256 MiB of RAM at virtual 0xC0000000 (kernel alias).
	 *   [0xF00..0xF1F] 32 MiB of peripherals at virtual 0xF0000000
	 *                  (physical 0x20000000), device memory.
	 *   [0xFFF]        1 MiB section -> physical 0x00000000 mapped at
	 *                  virtual 0xFFF00000, which puts the architectural
	 *                  high-vector address 0xFFFF0000 on the reserved
	 *                  vector page at physical 0x000F0000.
	 */
	ldr	r4, =L1_TABLE_PA

	/* [0x000]: temporary identity mapping of the first MiB. */
	ldr	r0, =L1_SECT_KERNEL_ROM
	str	r0, [r4, #0]

	/* [0xC00..0xCFF]: kernel RAM alias, 256 MiB.  mm/vmm.c extends this
	 * window to cover all of RAM once the real size is known. */
	ldr	r1, =L1_SECT_KERNEL_ROM
	mov	r2, #0
	mov	r3, #0xC00
	ldr	r5, =256
1:	orr	r6, r1, r2
	str	r6, [r4, r3, lsl #2]
	add	r2, r2, #0x00100000
	add	r3, r3, #1
	subs	r5, r5, #1
	bne	1b

	/* [0xF00..0xF1F]: peripheral window (device memory). */
	ldr	r1, =L1_SECT_DEVICE
	ldr	r2, =0x20000000
	mov	r3, #0xF00
	mov	r5, #32
1:	orr	r6, r1, r2
	str	r6, [r4, r3, lsl #2]
	add	r2, r2, #0x00100000
	add	r3, r3, #1
	subs	r5, r5, #1
	bne	1b

	/* [0xFFF]: high vectors alias (physical 0x00000000 -> 0xFFF00000). */
	ldr	r0, =L1_SECT_KERNEL_ROM
	ldr	r3, =0xFFF
	str	r0, [r4, r3, lsl #2]

	/* Translation table base, domain access control.  All 16 domains are
	 * configured as "client" so that the AP bits in the descriptors are
	 * the only access control in play; LumeOS uses domain 0 for
	 * everything. */
	ldr	r0, =L1_TABLE_PA
	mcr	p15, 0, r0, c2, c0, 0	@ TTBR0
	ldr	r0, =0x55555555		@ 01 = client for every domain
	mcr	p15, 0, r0, c3, c0, 0	@ DACR

	/* Enable the MMU, alignment checking, caches, branch prediction and
	 * high vectors in one go. */
	mrc	p15, 0, r0, c1, c0, 0
	orr	r0, r0, #SCTLR_M
	orr	r0, r0, #SCTLR_A
	orr	r0, r0, #SCTLR_C
	orr	r0, r0, #SCTLR_I
	orr	r0, r0, #SCTLR_V
	orr	r0, r0, #SCTLR_Z
	bic	r0, r0, #SCTLR_U	@ unaligned accesses trap (A=1)
	mcr	p15, 0, r0, c1, c0, 0
	mcr	p15, 0, r0, c7, c10, 4	@ DSB: ensure the table and SCTLR writes land
	mcr	p15, 0, r0, c7, c5, 4	@ ISB: flush the pipeline

	/* Branch to the high alias.  The literal below is the link-time
	 * virtual address of _start_high; because its physical image is
	 * mapped there by the table we just built, the fetch succeeds. */
	ldr	r0, =_start_high
	mov	pc, r0

	.balign 32
_start_high:
	/* We are now executing from 0xC0008xxx.  Drop the temporary identity
	 * mapping of the first MiB, which would otherwise collide with the
	 * user address space (user processes map 0x00000000 upward). */
	ldr	r4, =L1_TABLE_PA
	mov	r5, #0
	str	r5, [r4]
	mcr	p15, 0, r5, c8, c7, 0	@ TLBIALL: forget the stale entry
	mcr	p15, 0, r5, c7, c10, 4	@ DSB

	/* Zero the .bss section (register-only loop, no stack needed yet). */
	ldr	r4, =__bss_start
	ldr	r6, =__bss_end
	mov	r5, #0
1:	cmp	r4, r6
	strlo	r5, [r4], #4
	blo	1b

	/* Per-mode stacks.  IRQ/FIQ/ABT/UND get small stacks: exception
	 * entry points are only ever a few frames deep. */
	ldr	r1, =__stack_irq_top
	ldr	r2, =__stack_fiq_top
	ldr	r3, =__stack_abt_top
	ldr	r4, =__stack_und_top
	ldr	r5, =__stack_svc_top

	cps	#0x12			@ IRQ
	mov	sp, r1
	cps	#0x11			@ FIQ
	mov	sp, r2
	cps	#0x17			@ ABT
	mov	sp, r3
	cps	#0x1B			@ UND
	mov	sp, r4
	cps	#0x13			@ back to SVC
	mov	sp, r5

	/* Hand over to the C startup code:
	 *   r0 = physical address of the firmware-provided fdt/atags (or 0)
	 *   r1 = the physical address this kernel was really loaded at
	 */
	mov	r0, r9
	mov	r1, r10
	bl	kernel_main

	/* kernel_main never returns; if it does, park the CPU. */
1:	wfi
	b	1b
	.size _start, . - _start

	/*
	 * Exception vector table template, copied to physical 0x000F0000.
	 * Each entry is "ldr pc, [pc, #0x18]", which loads the corresponding
	 * literal from the end of the 64-byte block (offset 0x20 + 4*i).
	 * The literals are link-time virtual addresses and are therefore only
	 * used once the MMU is on, which is exactly when they are needed.
	 */
	.balign 32
	.global _vector_template
_vector_template:
	.rept 8
	ldr	pc, [pc, #0x18]
	.endr
	.word	vector_reset
	.word	vector_und
	.word	vector_svc
	.word	vector_pabt
	.word	vector_dabt
	.word	vector_reserved
	.word	vector_irq
	.word	vector_fiq
	.size _vector_template, . - _vector_template
