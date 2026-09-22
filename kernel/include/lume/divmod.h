/*
 * LumeOS portable integer division cores.
 *
 * These are the routines behind the ARM EABI helpers (__aeabi_uidiv and
 * friends).  They live here, outside the EABI glue, for two reasons:
 *
 *  1. They are pure arithmetic with no ARM-specific detail, so they can be
 *     compiled and unit-tested on the development host (tests/host), which the
 *     assembly thunks cannot be.
 *
 *  2. Their signatures use only 32-bit scalars and pointers.  That is
 *     deliberate: this toolchain returns *every* composite type through
 *     memory with a hidden pointer (sret) instead of in r0-r3, so a helper
 *     written as `struct pair f(...)` would not have the register layout its
 *     callers (or the EABI) expect.  Keeping word-sized arguments and output
 *     pointers makes the convention unambiguous at every level.
 *
 * Multi-word operands are passed as little-endian 32-bit word arrays: for a
 * 64-bit value, word[0] is the low half and word[1] the high half.  The arrays
 * must be 8-byte aligned, which stack slots created for that purpose are.
 *
 * Division by zero has no defined result.  The cores call lume_div_by_zero()
 * (supplied by the caller's environment) and return zero for both quotient and
 * remainder; the kernel turns that hook into a panic, while the host tests
 * record it.
 */
#ifndef LUME_DIVMOD_H
#define LUME_DIVMOD_H

#include <lume/types.h>

/* quotient in *quot, remainder in *rem */
void lume_udivmod32(u32 num, u32 den, u32 *quot, u32 *rem);
void lume_idivmod32(s32 num, s32 den, s32 *quot, s32 *rem);

/* out[0..1] = quotient (low, high), out[2..3] = remainder (low, high) */
void lume_udivmod64(const u32 num[2], const u32 den[2], u32 out[4]);
void lume_ldivmod64(const u32 num[2], const u32 den[2], u32 out[4]);

/* Called with a short description of the width ("32-bit", "64-bit") when a
 * divisor is zero.  Must not return a result; the cores return zero. */
void lume_div_by_zero(const char *width);

#endif /* LUME_DIVMOD_H */
