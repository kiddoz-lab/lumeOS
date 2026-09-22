/*
 * LumeOS generic time keeping.
 *
 * Thin layer on top of the architecture timer: monotonic time, a settable
 * wall clock, and busy-wait helpers for the early boot path.
 */
#include <lume/asm.h>
#include <lume/klog.h>
#include <lume/sched.h>
#include <lume/time.h>
#include <lume/types.h>

static u64 wall_offset_us;
static int wall_clock_set;
static int clock_running;

u64 time_monotonic_us(void)
{
    return timer_read_us();
}

u64 time_monotonic_ms(void)
{
    return timer_read_us() / 1000u;
}

u32 time_uptime_seconds(void)
{
    return (u32)(timer_read_us() / 1000000u);
}

void time_set_wall(u64 unix_us)
{
    wall_offset_us = unix_us - timer_read_us();
    wall_clock_set = 1;
    pr_info("time: wall clock set (epoch offset %lld us)", (long long)wall_offset_us);
}

u64 time_wall_us(void)
{
    return timer_read_us() + wall_offset_us;
}

int time_is_set(void)
{
    return wall_clock_set;
}

void udelay(u32 usec)
{
    u64 start = timer_read_us();
    u64 target = start + usec;

    /* Bounded wait: even if the counter misbehaves we never spin forever. */
    while (timer_read_us() < target) {
        /* If a tick is overdue while we wait (interrupts masked), account for
         * it so the scheduler does not see time jump backwards later. */
        timer_poll_fallback();
        arm_isb();
    }
}

void mdelay(u32 msec)
{
    while (msec--)
        udelay(1000);
}

int time_is_running(void)
{
    return clock_running;
}

void time_init(void)
{
    wall_offset_us = 0;
    wall_clock_set = 0;
    clock_running = 1;
    pr_info("time: monotonic clock from the system timer, wall clock unset "
            "(no RTC on this board)");
}
