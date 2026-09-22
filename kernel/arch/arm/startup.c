/*
 * LumeOS ARM/BCM2835 startup glue.
 *
 * Everything in this file is specific to the Raspberry Pi / BCM2835 and is
 * deliberately kept out of the portable sources in kernel/kernel so that the
 * kernel stays portable (see docs/architecture.md, "Portability boundary").
 */
#include <lume/asm.h>
#include <lume/config.h>
#include <lume/gpio.h>
#include <lume/hw/bcm2835.h>
#include <lume/input.h>
#include <lume/irq.h>
#include <lume/memdetect.h>
#include <lume/klog.h>
#include <lume/mbox.h>
#include <lume/panic.h>
#include <lume/time.h>
#include <lume/types.h>
#include <lume/uart.h>

/* Firmware data handed over by the boot stub. */
u32 arch_fdt_pointer;
u32 arch_load_address;

static void uart_console_putc(char c)
{
    /* The console converts LF to CRLF: the PL011 has no terminal emulation of
     * its own and every serial terminal expects CRLF at 115200 8N1. */
    if (c == '\n')
        uart_putc('\r');
    uart_putc(c);
}

void arch_early_init(u32 fdt_pa, u32 load_addr)
{
    arch_fdt_pointer = fdt_pa;
    arch_load_address = load_addr;

    /* Serial first: until this works there is no way to debug anything else. */
    uart_init(LUME_UART_DEFAULT_BAUD);
    console_register("uart0", uart_console_putc);

    mbox_init();
    memdetect_set_fdt(fdt_pa);
}

void arch_devices_init(void)
{
    gpio_init();
    irq_init();
    timer_init(LUME_HZ);
    timer_register_irq();
}

static void uart_rx_irq(u32 irq, void *arg)
{
    (void)irq;
    (void)arg;
    uart_irq_handler();
}

void arch_console_enable_input(void)
{
    input_init();
    input_start_polling();
    uart_enable_rx_irq(1);
    irq_register(IRQ_PL011, "uart0-rx", uart_rx_irq, NULL);
}

/* ------------------------------------------------------------------ */
/* SoC control: reboot through the watchdog block, halt forever.       */
/* ------------------------------------------------------------------ */
/* The watchdog registers live in the "power management" block at
 * 0x7E100000 (BCM2835 ARM Peripherals chapter 13, "Power Management"): the
 * watchdog is at +0x24 and the reset control at +0x1C.  Writes need the
 * 0x5A000000 password in the top byte.  Requesting a full reset is the
 * documented way to reboot a Raspberry Pi from software. */
#define PM_RSTC_WRCFG_MASK 0x30u   /* bits 5:4 select the reset behaviour */

void arch_reboot(void)
{
    volatile u32 *pm = (volatile u32 *)PHYS_TO_VIRT(BCM2835_PM_BASE);
    u32 rstc;

    arm_irq_disable();

    kputs("LumeOS: rebooting through the watchdog\n");

    /* Program the watchdog to fire almost immediately, then ask for a full
     * reset.  The delay field counts in units of 65536 clock cycles; 10 is
     * the value used by the firmware's own reboot path. */
    pm[PM_WDOG / 4] = PM_PASSWORD | 10;
    rstc = pm[PM_RSTC / 4];
    rstc = (rstc & ~PM_RSTC_WRCFG_MASK) | PM_RSTC_WRCFG_FULL_RESET;
    pm[PM_RSTC / 4] = PM_PASSWORD | rstc;

    arm_dsb();

    for (;;)
        arm_wfi();
}

void arch_poweroff(void)
{
    /* A Raspberry Pi Zero W has no software-controlled power switch.  The
     * honest implementation is to halt the CPU with interrupts masked and say
     * so; a real power-off would need external hardware (a GPIO-controlled
     * regulator, for example). */
    arm_irq_disable();
    kputs("LumeOS: system halted (this board cannot switch its own power)\n");
    for (;;)
        arm_wfi();
}
