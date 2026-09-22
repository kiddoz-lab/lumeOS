/*
 * LumeOS ARM EABI runtime support (the "libgcc subset").
 *
 * The kernel is linked with -nostdlib, so the helper routines the compiler
 * expects to find (AAPCS "rtabi" helpers) have to be provided:
 *
 *   - division:  __aeabi_uidiv, __aeabi_idiv      (here)
 *                __aeabi_uidivmod, __aeabi_idivmod,
 *                __aeabi_uldivmod, __aeabi_ldivmod (kernel/arch/arm/aeabi_div.S)
 *   - memory:    __aeabi_memcpy/memmove/memset/memclr and the 4/8 alignment
 *                variants (here)
 *
 * ARM1176JZF-S has no divide instruction, so the arithmetic behind all of
 * these is the restoring division loop in kernel/kernel/divmod.c, which is
 * portable C and therefore unit-tested on the development host.
 *
 * IMPORTANT (this cost one boot in QEMU):
 *
 * The rtabi helper names have a fixed register-level calling convention taken
 * from the public "Run-time ABI for the ARM Architecture" addendum, and that
 * convention is *not* whatever the C compiler would pick for an equivalent C
 * function.  In particular clang/LLVM for this target returns every composite
 * type in memory through a hidden pointer (sret), even a two-word struct, so
 * writing __aeabi_uldivmod as a C function returning `struct {u64,u64}` gives
 * a routine that reads its divisor from the stack and writes the result
 * through r0 - while its callers pass the operands in r0-r3 and read the
 * results back from r0-r3.  The mis-read divisor happened to be zero and the
 * kernel panicked with "64-bit division by zero" before the console existed.
 *
 * Hence: scalar in / scalar out helpers stay in C, and anything whose rtabi
 * convention is fixed and multi-register lives in assembly
 * (kernel/arch/arm/aeabi_div.S).
 *
 * The memory helpers use byte loops on purpose: the compiler is allowed to
 * turn a "copy n bytes" loop into a call to __aeabi_memcpy, which would
 * recurse forever.  -fno-builtin already prevents that, but these helpers must
 * not depend on it.
 */
#include <lume/divmod.h>
#include <lume/panic.h>
#include <lume/types.h>

/* ------------------------------------------------------------------ */
/* 32-bit division                                                     */
/* ------------------------------------------------------------------ */

u32 __aeabi_uidiv(u32 num, u32 den);
u32 __aeabi_uidiv(u32 num, u32 den)
{
    u32 quot, rem;

    lume_udivmod32(num, den, &quot, &rem);
    return quot;
}

s32 __aeabi_idiv(s32 num, s32 den);
s32 __aeabi_idiv(s32 num, s32 den)
{
    s32 quot, rem;

    lume_idivmod32(num, den, &quot, &rem);
    return quot;
}

/* Policy hook used by the portable cores: a zero divisor has no defined
 * result, so the kernel treats it as a bug. */
void lume_div_by_zero(const char *width);
void lume_div_by_zero(const char *width)
{
    panic("division by zero (%s)", width);
}

/* ------------------------------------------------------------------ */
/* Memory helpers                                                      */
/* ------------------------------------------------------------------ */

void __aeabi_memcpy(void *dst, const void *src, u32 n);
void __aeabi_memcpy(void *dst, const void *src, u32 n)
{
    u8 *d = dst;
    const u8 *s = src;
    while (n--)
        *d++ = *s++;
}

void __aeabi_memcpy4(void *dst, const void *src, u32 n);
void __aeabi_memcpy4(void *dst, const void *src, u32 n)
{
    __aeabi_memcpy(dst, src, n);
}

void __aeabi_memcpy8(void *dst, const void *src, u32 n);
void __aeabi_memcpy8(void *dst, const void *src, u32 n)
{
    __aeabi_memcpy(dst, src, n);
}

void __aeabi_memmove(void *dst, const void *src, u32 n);
void __aeabi_memmove(void *dst, const void *src, u32 n)
{
    u8 *d = dst;
    const u8 *s = src;

    if (d == s || n == 0)
        return;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
}

void __aeabi_memmove4(void *dst, const void *src, u32 n);
void __aeabi_memmove4(void *dst, const void *src, u32 n)
{
    __aeabi_memmove(dst, src, n);
}

void __aeabi_memmove8(void *dst, const void *src, u32 n);
void __aeabi_memmove8(void *dst, const void *src, u32 n)
{
    __aeabi_memmove(dst, src, n);
}

void __aeabi_memset(void *dst, u32 n, int c);
void __aeabi_memset(void *dst, u32 n, int c)
{
    u8 *d = dst;
    while (n--)
        *d++ = (u8)c;
}

void __aeabi_memset4(void *dst, u32 n, int c);
void __aeabi_memset4(void *dst, u32 n, int c)
{
    __aeabi_memset(dst, n, c);
}

void __aeabi_memset8(void *dst, u32 n, int c);
void __aeabi_memset8(void *dst, u32 n, int c)
{
    __aeabi_memset(dst, n, c);
}

void __aeabi_memclr(void *dst, u32 n);
void __aeabi_memclr(void *dst, u32 n)
{
    __aeabi_memset(dst, n, 0);
}

void __aeabi_memclr4(void *dst, u32 n);
void __aeabi_memclr4(void *dst, u32 n)
{
    __aeabi_memset(dst, n, 0);
}

void __aeabi_memclr8(void *dst, u32 n);
void __aeabi_memclr8(void *dst, u32 n)
{
    __aeabi_memset(dst, n, 0);
}
