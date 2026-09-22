/*
 * LumeOS portable integer division cores.  See lume/divmod.h for why these are
 * not written directly as the EABI entry points.
 *
 * ARM1176JZF-S has no divide instruction, so everything here is restoring
 * (shift-and-subtract) division.  The loops shift one bit of the dividend into
 * the remainder per iteration and subtract the divisor whenever the remainder
 * is large enough; the carry out of the remainder shift is folded in so a
 * remainder that overflows 32 (or 64) bits still compares correctly.
 */
#include <lume/divmod.h>
#include <lume/types.h>

void lume_udivmod32(u32 num, u32 den, u32 *quot, u32 *rem)
{
    u32 q = 0;
    u32 r = 0;

    if (den == 0) {
        lume_div_by_zero("32-bit");
        *quot = 0;
        *rem = 0;
        return;
    }

    for (int bit = 31; bit >= 0; bit--) {
        u32 carry = r >> 31;                  /* bit shifted out of the remainder */

        q <<= 1;
        r = (r << 1) | ((num >> bit) & 1u);
        if (carry || r >= den) {
            /* Subtracting in 32-bit arithmetic is still correct when the
             * remainder overflowed: the true value is at most 2*den-1, so the
             * low 32 bits of (true - den) are the real remainder. */
            r -= den;
            q |= 1u;
        }
    }

    *quot = q;
    *rem = r;
}

void lume_idivmod32(s32 num, s32 den, s32 *quot, s32 *rem)
{
    u32 unum = (u32)num;
    u32 uden = (u32)den;
    u32 uq, ur;
    int quot_negative = 0;

    if (num < 0) {
        unum = (u32)0 - unum;
        quot_negative = !quot_negative;
    }
    if (den < 0) {
        uden = (u32)0 - uden;
        quot_negative = !quot_negative;
    }

    lume_udivmod32(unum, uden, &uq, &ur);

    *quot = quot_negative ? (s32)((u32)0 - uq) : (s32)uq;
    /* C99: the remainder takes the sign of the dividend. */
    *rem = (num < 0) ? (s32)((u32)0 - ur) : (s32)ur;
}

void lume_udivmod64(const u32 num[2], const u32 den[2], u32 out[4])
{
    u64 n = ((u64)num[1] << 32) | (u64)num[0];
    u64 d = ((u64)den[1] << 32) | (u64)den[0];
    u64 q = 0;
    u64 r = 0;
    int top = 63;

    if (d == 0) {
        lume_div_by_zero("64-bit");
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }

    while (top > 0 && ((n >> top) & 1u) == 0)
        top--;

    for (int bit = top; bit >= 0; bit--) {
        u64 carry = r >> 63;

        q <<= 1;
        r = (r << 1) | ((n >> bit) & 1u);
        if (carry || r >= d) {
            r -= d;
            q |= 1u;
        }
    }

    out[0] = (u32)q;
    out[1] = (u32)(q >> 32);
    out[2] = (u32)r;
    out[3] = (u32)(r >> 32);
}

void lume_ldivmod64(const u32 num[2], const u32 den[2], u32 out[4])
{
    u64 n = ((u64)num[1] << 32) | (u64)num[0];
    u64 d = ((u64)den[1] << 32) | (u64)den[0];
    int quot_negative = 0;
    int num_negative = (num[1] & 0x80000000u) != 0;
    u32 unum[2], uden[2];

    if (num_negative) {
        u64 mag = (u64)0 - n;
        unum[0] = (u32)mag;
        unum[1] = (u32)(mag >> 32);
        quot_negative = !quot_negative;
    } else {
        unum[0] = num[0];
        unum[1] = num[1];
    }

    if (den[1] & 0x80000000u) {
        u64 mag = (u64)0 - d;
        uden[0] = (u32)mag;
        uden[1] = (u32)(mag >> 32);
        quot_negative = !quot_negative;
    } else {
        uden[0] = den[0];
        uden[1] = den[1];
    }

    lume_udivmod64(unum, uden, out);

    if (quot_negative) {
        u64 q = (u64)0 - (((u64)out[1] << 32) | (u64)out[0]);
        out[0] = (u32)q;
        out[1] = (u32)(q >> 32);
    }
    if (num_negative) {
        u64 r = (u64)0 - (((u64)out[3] << 32) | (u64)out[2]);
        out[2] = (u32)r;
        out[3] = (u32)(r >> 32);
    }
}
