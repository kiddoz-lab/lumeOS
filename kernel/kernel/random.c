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

/*
 * Seeding *replaces* the state: the same argument always produces the same
 * stream.  That is a deliberate property, not an oversight.  A generator that
 * mixes the previous state into every seed cannot be tested by asking "seed
 * with X, take 16 bytes, seed with X again, do you get the same 16 bytes?" -
 * which is exactly how the self test proves that the bytes AT_RANDOM points at
 * in a user stack are the generator's output rather than whatever the page
 * happened to contain.  Callers who want extra entropy pass it in the argument:
 * rng_ensure_seeded() mixes the microsecond counter, the tick count, the
 * kernel's load address and the current pid before calling this.
 */
void lume_random_seed(u32 entropy)
{
    u32 x = entropy ^ 0x9E3779B9u;

    /* A few rounds of the generator itself, plus a multiplication, so that a
     * seed with long runs of zero or one bits (a small tick count, say) still
     * produces a state with a well-spread bit pattern. */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
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

    /* Two 32-bit words of the counter plus the tick count, the load address and
     * the pid, folded into the single argument this takes. */
    lume_random_seed(mix ^ (u32)(us >> 13));
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
