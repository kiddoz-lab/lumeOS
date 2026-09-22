/*
 * LumeOS ARM EABI division helpers - the register-level entry points.
 *
 * The compiler emits calls to these symbols for C division and modulo
 * (ARM1176JZF-S has no divide instruction).  Their calling convention is fixed
 * by the public "Run-time ABI for the ARM Architecture" addendum and is NOT
 * the convention a C compiler would choose for an equivalent C function: this
 * toolchain returns composite types in memory through a hidden pointer (sret),
 * so the same routines written in C read their divisor from the stack and
 * return into a pointer that r0 does not actually hold.  See
 * kernel/kernel/aeabi.c for the full story; that mismatch panicked the kernel
 * before the serial console existed and is what these thunks replace.
 *
 * Fixed conventions (r = register):
 *
 *   __aeabi_uidivmod(u32 num, u32 den)   num r0, den r1
 *                                        -> quotient r0, remainder r1
 *   __aeabi_idivmod (s32 num, s32 den)   num r0, den r1
 *                                        -> quotient r0, remainder r1
 *   __aeabi_uldivmod(u64 num, u64 den)   num r0:r1, den r2:r3
 *                                        -> quotient r0:r1, remainder r2:r3
 *   __aeabi_ldivmod (s64 num, s64 den)   num r0:r1, den r2:r3
 *                                        -> quotient r0:r1, remainder r2:r3
 *
 * All arithmetic is done by the portable cores in kernel/kernel/divmod.c,
 * whose signatures use only word-sized scalars and pointers and are therefore
 * unambiguous.  The thunks only marshal registers, and they keep the stack
 * 8-byte aligned as AAPCS requires.
 */
    .syntax unified
    .arm
    .text

/* ---- __aeabi_uidivmod / __aeabi_idivmod -------------------------------- */
/* void lume_udivmod32(u32 num, u32 den, u32 *quot, u32 *rem)
 *   r0 = num, r1 = den, r2 = &quot, r3 = &rem                                */
    .global __aeabi_uidivmod
    .type   __aeabi_uidivmod, %function
__aeabi_uidivmod:
    push    {r4, lr}
    sub     sp, sp, #8
    mov     r2, sp                  /* &quot  */
    add     r3, sp, #4              /* &rem  */
    bl      lume_udivmod32
    ldr     r0, [sp]
    ldr     r1, [sp, #4]
    add     sp, sp, #8
    pop     {r4, pc}
    .size   __aeabi_uidivmod, . - __aeabi_uidivmod

    .global __aeabi_idivmod
    .type   __aeabi_idivmod, %function
__aeabi_idivmod:
    push    {r4, lr}
    sub     sp, sp, #8
    mov     r2, sp
    add     r3, sp, #4
    bl      lume_idivmod32
    ldr     r0, [sp]
    ldr     r1, [sp, #4]
    add     sp, sp, #8
    pop     {r4, pc}
    .size   __aeabi_idivmod, . - __aeabi_idivmod

/* ---- __aeabi_uldivmod --------------------------------------------------- */
/* void lume_udivmod64(const u32 num[2], const u32 den[2], u32 out[4])
 *   r0 = num, r1 = den, r2 = out
 * Stack frame: [sp+0..7] num, [sp+8..15] den, [sp+16..31] quotient/remainder.
 * 8-byte aligned, which the u64 operands in the core require.               */
    .global __aeabi_uldivmod
    .type   __aeabi_uldivmod, %function
__aeabi_uldivmod:
    push    {r4, lr}
    sub     sp, sp, #32
    str     r0, [sp]
    str     r1, [sp, #4]
    str     r2, [sp, #8]
    str     r3, [sp, #12]
    mov     r0, sp                  /* &num  */
    add     r1, sp, #8              /* &den  */
    add     r2, sp, #16             /* &out  */
    bl      lume_udivmod64
    ldr     r0, [sp, #16]           /* quot low  */
    ldr     r1, [sp, #20]           /* quot high */
    ldr     r2, [sp, #24]           /* rem low   */
    ldr     r3, [sp, #28]           /* rem high  */
    add     sp, sp, #32
    pop     {r4, pc}
    .size   __aeabi_uldivmod, . - __aeabi_uldivmod

/* ---- __aeabi_ldivmod ---------------------------------------------------- */
    .global __aeabi_ldivmod
    .type   __aeabi_ldivmod, %function
__aeabi_ldivmod:
    push    {r4, lr}
    sub     sp, sp, #32
    str     r0, [sp]
    str     r1, [sp, #4]
    str     r2, [sp, #8]
    str     r3, [sp, #12]
    mov     r0, sp
    add     r1, sp, #8
    add     r2, sp, #16
    bl      lume_ldivmod64
    ldr     r0, [sp, #16]
    ldr     r1, [sp, #20]
    ldr     r2, [sp, #24]
    ldr     r3, [sp, #28]
    add     sp, sp, #32
    pop     {r4, pc}
    .size   __aeabi_ldivmod, . - __aeabi_ldivmod
