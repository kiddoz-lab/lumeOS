/*
 * LumeOS BCM2835 system timer driver.
 *
 * Hardware (BCM2835 ARM Peripherals chapter 12 "System Timer"):
 *   base 0x7E003000 (0x20003000 from the ARM)
 *   +0x00 CS   control and status; write 1 to a bit to clear that match flag
 *   +0x04 CLO  lower 32 bits of the free-running 1 MHz counter
 *   +0x08 CHI  upper 32 bits
 *   +0x0C..+0x18 C0..C3 compare registers; an interrupt is raised when the
 *                counter reaches the compare value
 *
 * Compare channels 0 and 2 are documented as belonging to the GPU firmware,
 * and channel 1 is the one the firmware's own drivers prefer, so LumeOS uses
 * compare channel 3 for its scheduler tick.
 *
 * A note on interrupt routing.  The four compare channels appear as interrupt
 * numbers 0..3 in the BCM2835 interrupt table and are reported through the
 * "IRQ pending 1" register.  Because a wrong guess there would silently stop
 * the scheduler tick, the driver never trusts the routing blindly:
 *
 *   - the IRQ handler always checks the timer's own CS register and services
 *     the tick when compare 3 has matched, whatever bit caused the interrupt;
 *   - timer_poll_fallback() lets the idle loop service an overdue tick
 *     directly from the counter if no interrupt has arrived.  The in-kernel
 *     self test reports which path does the work, so the boot log on real
 *     hardware immediately shows whether the IRQ routing matches this
 *     description.
 */
#include <lume/asm.h>
#include <lume/config.h>
#include <lume/irq.h>
#include <lume/hw/bcm2835.h>
#include <lume/klog.h>
#include <lume/panic.h>
#include <lume/sched.h>
#include <lume/time.h>
#include <lume/types.h>

#define TIMER_REG(off) (*(volatile u32 *)((u32)PHYS_TO_VIRT(BCM2835_SYSTIMER_BASE) + (off)))

#define TIMER_CS  0x00
#define TIMER_CLO 0x04
#define TIMER_CHI 0x08
#define TIMER_C1  0x10
#define TIMER_C3  0x18

#define TIMER_CS_M3 (1u << 3)

static u32 tick_hz = LUME_HZ;
static u32 tick_period_us;
static u64 next_tick_us;
static volatile u64 ticks;
static volatile u64 polled_ticks;
static int timer_ready;

u64 timer_read_us(void)
{
    u32 hi, lo, hi2;

    /* The two halves are not read atomically: read high, low, high again and
     * retry when the high word changed in between (the documented sequence
     * for this timer). */
    do {
        hi = TIMER_REG(TIMER_CHI);
        lo = TIMER_REG(TIMER_CLO);
        hi2 = TIMER_REG(TIMER_CHI);
    } while (hi != hi2);

    return ((u64)hi << 32) | lo;
}

static void timer_program_next(u64 now)
{
    next_tick_us += tick_period_us;
    if (next_tick_us <= now)
        next_tick_us = now + tick_period_us;
    TIMER_REG(TIMER_C3) = (u32)next_tick_us;
}

static void timer_service_tick(void)
{
    TIMER_REG(TIMER_CS) = TIMER_CS_M3;   /* clear the match flag */
    timer_program_next(timer_read_us());
    ticks++;
    sched_tick();
}

void timer_irq_handler(void)
{
    if (TIMER_REG(TIMER_CS) & TIMER_CS_M3)
        timer_service_tick();
}

u32 timer_poll_fallback(void)
{
    u32 serviced = 0;

    while (timer_ready && timer_read_us() >= next_tick_us) {
        timer_service_tick();
        polled_ticks++;
        serviced++;
        if (serviced > 32)
            break;
    }
    return serviced;
}

static void systimer_irq(u32 irq, void *arg)
{
    (void)irq;
    (void)arg;
    timer_irq_handler();
}

void timer_register_irq(void)
{
    irq_register(IRQ_SYSTIMER_C3, "systimer", systimer_irq, NULL);
    pr_debug("timer: registered on IRQ %u", IRQ_SYSTIMER_C3);
}

void timer_init(u32 hz)
{
    if (hz == 0)
        hz = LUME_HZ;
    tick_hz = hz;
    tick_period_us = 1000000u / hz;
    if (tick_period_us == 0)
        tick_period_us = 1;

    /* Start from a clean slate: clear any pending match and program the next
     * tick relative to now. */
    TIMER_REG(TIMER_CS) = 0xFu;
    next_tick_us = timer_read_us() + tick_period_us;
    TIMER_REG(TIMER_C3) = (u32)next_tick_us;
    timer_ready = 1;

    pr_info("timer: BCM2835 system timer at 1 MHz, %u Hz tick on compare 3 "
            "(counter=%llu us)", tick_hz, timer_read_us());
}

u32 timer_tick_hz(void)
{
    return tick_hz;
}

u64 timer_ticks(void)
{
    return ticks;
}

u64 timer_polled_ticks(void)
{
    return polled_ticks;
}
