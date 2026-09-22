/*
 * LumeOS fatal error handling.
 */
#include <lume/asm.h>
#include <lume/config.h>
#include <lume/hw/bcm2835.h>
#include <lume/klog.h>
#include <lume/panic.h>
#include <lume/string.h>

static int panicking;

void panic(const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    arm_irq_disable();
    panicking = 1;

    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    kputs("\n");
    kputs("================================================================\n");
    kputs("LumeOS kernel panic: ");
    kputs(msg);
    kputs("\n");
    kputs("The kernel is halted.  Reset the board to boot again.\n");
    kputs("(Under QEMU use the monitor 'quit' command or Ctrl-A x.)\n");
    kputs("================================================================\n");

    for (;;)
        arm_wfi();
}

/* Called from the assembly vector stubs before the C runtime is enterable in
 * some cases (reset/reserved/FIQ). */
void panic_early(const char *msg, u32 lr)
{
    arm_irq_disable();
    panicking = 1;

    /* Everything here is deliberately simple: no formatting, no locking. */
    kputs("\nLumeOS early panic: ");
    kputs(msg ? msg : "(no message)");
    kputs("\n  lr = 0x");
    char buf[9];
    u32 v = lr;
    for (int i = 7; i >= 0; i--) {
        u32 d = v & 0xF;
        buf[i] = (d < 10) ? (char)('0' + d) : (char)('a' + d - 10);
        v >>= 4;
    }
    buf[8] = 0;
    kputs(buf);
    kputs("\n");

    for (;;)
        arm_wfi();
}

int panic_pending(void)
{
    return panicking;
}

/* Force a full SoC reset through the watchdog block.  Used by
 * LUME_SYS_REBOOT and by the shell's "reboot" built-in. */
void lume_reboot(void)
{
    volatile u32 *pm = (volatile u32 *)PHYS_TO_VIRT(BCM2835_PM_BASE);

    kputs("LumeOS: rebooting\n");
    arm_dsb();

    pm[BCM2835_WDOG_OFFSET / 4] = PM_PASSWORD | 10; /* 10 * 65536 / 2 clocks */
    pm[PM_RSTC / 4] = PM_PASSWORD | PM_RSTC_WRCFG_FULL_RESET;

    for (;;)
        arm_wfi();
}

void lume_poweroff(void)
{
    /* The Pi Zero W has no power controller that can cut its own power; the
     * honest implementation is to halt with interrupts disabled and let the
     * operator remove power.  LUME_SYS_POWEROFF reports this to userspace. */
    kputs("LumeOS: halted (power off requires removing power)\n");
    arm_irq_disable();
    for (;;)
        arm_wfi();
}
