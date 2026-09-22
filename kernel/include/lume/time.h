/*
 * LumeOS time keeping.
 *
 * The only time source on a Raspberry Pi Zero W is the BCM2835 system timer:
 * a free-running 64-bit counter that increments once per microsecond
 * (BCM2835 ARM Peripherals chapter 12).  There is no real-time clock, so the
 * wall clock starts at the Unix epoch plus whatever the boot procedure sets
 * (see the LumeOS syscall LUME_SYS_SETTIME and docs/hardware.md); a shell
 * reports that honestly instead of inventing a date.
 *
 * Two clocks are maintained:
 *   - monotonic: microseconds since boot, from the hardware counter;
 *   - wall clock: monotonic plus a settable offset (Unix epoch based).
 */
#ifndef LUME_TIME_H
#define LUME_TIME_H

#include <lume/types.h>

/* Microsecond timer value (64-bit, wrapping after ~584942 years). */
u64 timer_read_us(void);

void timer_init(u32 tick_hz);

/* True once the monotonic clock can be read (used by the logger to decide
 * whether a timestamp is meaningful yet). */
int time_is_running(void);

/* Generic time keeping initialisation. */
void time_init(void);
void timer_irq_handler(void);
u32 timer_tick_hz(void);

/* Register the timer's interrupt handler with the interrupt controller.  Kept
 * separate from timer_init() so the caller decides when interrupts may be
 * enabled. */
void timer_register_irq(void);

/* Tick counters, for diagnostics and the self test. */
u64 timer_ticks(void);
u64 timer_polled_ticks(void);

/* Monotonic time helpers. */
u64 time_monotonic_us(void);
u64 time_monotonic_ms(void);
u32 time_uptime_seconds(void);

/* Wall clock (Unix epoch microseconds). */
void time_set_wall(u64 unix_us);
u64 time_wall_us(void);
int  time_is_set(void);

/* Busy-wait helpers (usable before the scheduler exists). */
void udelay(u32 usec);
void mdelay(u32 msec);

/* Idle/fallback tick service.  Returns the number of ticks serviced.  See
 * arch/arm/timer.c for why this exists. */
u32 timer_poll_fallback(void);

#endif /* LUME_TIME_H */
