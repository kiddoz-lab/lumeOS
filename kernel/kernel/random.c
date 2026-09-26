/*
 * LumeOS random number generation - see kernel/include/lume/random.h for what
 * these bytes are and are not good for.
 *
 * xorshift32 (Marsaglia) because it is four instructions, has no table, and
 * cannot be called before the heap exists.  The state is seeded lazily from the
 * 64-bit microsecond counter, which is running before the timer driver is
 * initialised (the hardware counter free-runs from power-on), so this is safe
 * even if something asks for randomness very early.
 *
 * The whole generator is one u32 with no lock.  Callers today run with
 * interrupts masked (process creation does), and a hypothetical racing caller
 * could only mix two transitions together - the state can never become
 * inconsistent, because every step is a plain read-modify-write of one word.
 */
#include <lume/proc.h>
#include <lume/random.h>
#include <lume/time.h>

static u32 rng_state;
static int rng_seeded;

void lume_random_seed(u32 entropy)
{
    u32 x = rng_state ^ entropy;

    /* A few rounds of the generator itself, plus a multiplication, so that a
     * caller who seeds with something low-entropy (a tick count, a pid) does
     * not hand the generator a state with long runs of zero or one bits. */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    x *= 0x9E3779B1u;
    x ^= x >> 15;
    rng_state = x ? x : 0x1F123BB5u;   /* xorshift must never hold 0 */
    rng_seeded = 1;
}

static void rng_ensure_seeded(void)
{
    u64 us;
    struct process *p;
    u32 mix;

    if (rng_seeded)
        return;

    us = timer_read_us();
    p = proc_current();

    mix = (u32)us ^ (u32)(us >> 32) ^ (u32)timer_ticks();
    mix ^= (u32)(unsigned long)&rng_state;     /* where the kernel landed in RAM */
    mix ^= p ? p->pid : 0;

    rng_state = mix;
    lume_random_seed((u32)(us >> 13));
}

u32 lume_random_u32(void)
{
    u32 x;

    rng_ensure_seeded();

    x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

void lume_random_bytes(void *buf, u32 len)
{
    u8 *out = (u8 *)buf;
    u32 done = 0;

    while (done < len) {
        u32 word = lume_random_u32();

        for (u32 i = 0; i < 4 && done < len; i++, done++)
            out[done] = (u8)(word >> (8 * i));
    }
}
