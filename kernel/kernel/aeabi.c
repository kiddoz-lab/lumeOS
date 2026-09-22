/*
 * LumeOS ARM EABI runtime support ("libgcc" subset).
 *
 * The kernel is linked with -nostdlib, so the helper routines the compiler
 * expects to find (AAPCS "rtabi" helpers) have to be provided here:
 *
 *   - division:      __aeabi_uidiv, __aeabi_uidivmod, __aeabi_idiv,
 *                    __aeabi_idivmod, __aeabi_uldivmod, __aeabi_ldivmod
 *   - memory:        __aeabi_memcpy/memmove/memset/memclr and their
 *                    alignment-suffixed variants (4/8)
 *
 * Implementation notes:
 *
 *  - ARMv6 (ARM1176JZF-S) has no hardware divide, so integer division is a
 *    shift/subtract (restoring) loop.  The EABI returns a multi-word result in
 *    r0:r1 / r0-r3; the AAPCS returns a structure of up to four words in
 *    exactly those registers, so a structure return expresses the calling
 *    convention directly instead of needing hand-written assembly.
 *
 *  - The memory helpers use byte loops on purpose: the compiler is allowed to
 *    turn a "copy n bytes" loop into a call to __aeabi_memcpy, which would
 *    recurse forever.  -fno-builtin already prevents that, but these helpers
 *    must not depend on it.
 *
 * The helper names and register conventions come from the public "Run-time
 * ABI for the ARM Architecture" addendum, not from any operating system.
 */
#include <lume/panic.h>
#include <lume/types.h>

/* ------------------------------------------------------------------ */
/* 32-bit division                                                     */
/* ------------------------------------------------------------------ */

struct u32_divmod {
    u32 quot;
    u32 rem;
};

struct s32_divmod {
    s32 quot;
    s32 rem;
};

static struct u32_divmod udivmod32(u32 num, u32 den)
{
    struct u32_divmod r = { 0, 0 };

    if (den == 0)
        panic("division by zero");

    for (int bit = 31; bit >= 0; bit--) {
        u32 carry = r.rem >> 31;              /* bit shifted out of the remainder */

        r.quot <<= 1;
        r.rem = (r.rem << 1) | ((num >> bit) & 1u);
        if (carry || r.rem >= den) {
            /* Subtracting in 32-bit arithmetic is still correct when the
             * remainder overflowed: the true value is at most 2*den-1, so the
             * low 32 bits of (true - den) are the real remainder. */
            r.rem -= den;
            r.quot |= 1u;
        }
    }
    return r;
}

u32 __aeabi_uidiv(u32 num, u32 den);
u32 __aeabi_uidiv(u32 num, u32 den)
{
    return udivmod32(num, den).quot;
}

/* Quotient in r0, remainder in r1 (EABI convention). */
struct u32_divmod __aeabi_uidivmod(u32 num, u32 den);
struct u32_divmod __aeabi_uidivmod(u32 num, u32 den)
{
    return udivmod32(num, den);
}

s32 __aeabi_idiv(s32 num, s32 den);
s32 __aeabi_idiv(s32 num, s32 den)
{
    u32 unum = (u32)num, uden = (u32)den;
    int negative = 0;

    if (num < 0) {
        unum = (u32)0 - (u32)num;
        negative = !negative;
    }
    if (den < 0) {
        uden = (u32)0 - (u32)den;
        negative = !negative;
    }

    u32 quot = udivmod32(unum, uden).quot;
    return negative ? (s32)((u32)0 - quot) : (s32)quot;
}

struct s32_divmod __aeabi_idivmod(s32 num, s32 den);
struct s32_divmod __aeabi_idivmod(s32 num, s32 den)
{
    struct s32_divmod r;
    u32 unum = (u32)num, uden = (u32)den;
    int quot_negative = 0;

    if (num < 0) {
        unum = (u32)0 - (u32)num;
        quot_negative = !quot_negative;
    }
    if (den < 0) {
        uden = (u32)0 - (u32)den;
        quot_negative = !quot_negative;
    }

    struct u32_divmod u = udivmod32(unum, uden);

    r.quot = quot_negative ? (s32)((u32)0 - u.quot) : (s32)u.quot;
    /* C99: the remainder takes the sign of the dividend. */
    r.rem = (num < 0) ? (s32)((u32)0 - u.rem) : (s32)u.rem;
    return r;
}

/* ------------------------------------------------------------------ */
/* 64-bit division                                                     */
/* ------------------------------------------------------------------ */

struct u64_divmod {
    u64 quot;
    u64 rem;
};

struct s64_divmod {
    s64 quot;
    s64 rem;
};

static struct u64_divmod udivmod64(u64 num, u64 den)
{
    struct u64_divmod r = { 0, 0 };

    if (den == 0)
        panic("64-bit division by zero");

    int top = 63;
    while (top > 0 && ((num >> top) & 1u) == 0)
        top--;

    for (int bit = top; bit >= 0; bit--) {
        u64 carry = r.rem >> 63;

        r.quot <<= 1;
        r.rem = (r.rem << 1) | ((num >> bit) & 1u);
        if (carry || r.rem >= den) {
            r.rem -= den;
            r.quot |= 1u;
        }
    }
    return r;
}

/* Quotient in r0:r1, remainder in r2:r3 (EABI convention). */
struct u64_divmod __aeabi_uldivmod(u64 num, u64 den);
struct u64_divmod __aeabi_uldivmod(u64 num, u64 den)
{
    return udivmod64(num, den);
}

struct s64_divmod __aeabi_ldivmod(s64 num, s64 den);
struct s64_divmod __aeabi_ldivmod(s64 num, s64 den)
{
    struct s64_divmod r;
    u64 unum = (u64)num, uden = (u64)den;
    int quot_negative = 0;

    if (num < 0) {
        unum = (u64)0 - (u64)num;
        quot_negative = !quot_negative;
    }
    if (den < 0) {
        uden = (u64)0 - (u64)den;
        quot_negative = !quot_negative;
    }

    struct u64_divmod u = udivmod64(unum, uden);

    r.quot = quot_negative ? (s64)((u64)0 - u.quot) : (s64)u.quot;
    r.rem = (num < 0) ? (s64)((u64)0 - u.rem) : (s64)u.rem;
    return r;
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
