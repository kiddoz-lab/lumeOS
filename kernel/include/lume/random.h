/*
 * Random bytes - and an honest description of how random they are.
 *
 * The only consumer today is AT_RANDOM: 16 bytes on the initial stack that a C
 * library uses to seed its stack canary and its own PRNG.  That is a security
 * mechanism, so the quality of these bytes matters, and the truth is:
 *
 *   - this is a *deterministic generator* (xorshift32) seeded from the system
 *     timer, the tick counter, and the address of the stack it is filling.  It
 *     is enough to make a canary differ between processes and between boots.
 *   - it is NOT cryptographic entropy and must not be treated as such.  An
 *     attacker who can observe one process's canary can reconstruct the state
 *     and predict the next process's.
 *
 * The fix is not a better PRNG, it is a real source: the BCM2835 has a hardware
 * random number generator at 0x20104000 (BCM2835 ARM Peripherals chapter 15),
 * QEMU models it, and driving it is a small, self-contained piece of work - it
 * is listed in docs/roadmap.md rather than done here, because a kernel that
 * claims to seed a canary from a source it does not have would be worse than
 * one that says so.
 */
#ifndef LUME_RANDOM_H
#define LUME_RANDOM_H

#include <lume/types.h>

/** Mix `entropy` into the generator state. */
void lume_random_seed(u32 entropy);

/** Fill `buf` with `len` bytes.  See the file comment for what that means. */
void lume_random_bytes(void *buf, u32 len);

/** A non-zero 32-bit value, for use as a token (e.g. a canary of our own). */
u32 lume_random_u32(void);

#endif /* LUME_RANDOM_H */
