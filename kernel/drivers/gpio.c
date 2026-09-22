/*
 * LumeOS BCM2835 GPIO driver.
 *
 * Register layout: BCM2835 ARM Peripherals chapter 6.  The function select
 * registers hold 3 bits per pin, ten pins per register.  The pull-up/down
 * control uses the documented two-step sequence: write the control value to
 * GPPUD, wait 150 cycles, clock the value into the pin's GPPUDCLK bit, wait
 * another 150 cycles, then clear both registers (datasheet 6.2).
 */
#include <lume/asm.h>
#include <lume/gpio.h>
#include <lume/hw/bcm2835.h>
#include <lume/klog.h>
#include <lume/types.h>

#define GPIO_REG(off) (*(volatile u32 *)((u32)PERIPHERAL_TO_VIRT(BCM2835_GPIO_BASE) + (off)))

void gpio_init(void)
{
    /* Nothing to bring up globally: the firmware has already configured the
     * pins it needs (SD card, UART on ALT0).  Individual users set their own
     * pins through gpio_set_function(). */
}

int gpio_set_function(u32 pin, enum gpio_function func)
{
    if (pin >= LUME_GPIO_MAX)
        return -1;

    volatile u32 *fsel = (volatile u32 *)((u32)PERIPHERAL_TO_VIRT(BCM2835_GPIO_BASE) + GPIO_FSEL0);
    u32 reg = pin / 10;
    u32 shift = (pin % 10) * 3;
    u32 flags = arm_irq_save();

    fsel[reg] = (fsel[reg] & ~(7u << shift)) | (((u32)func & 7u) << shift);
    arm_dsb();
    arm_irq_restore(flags);
    return 0;
}

int gpio_set_pull(u32 pin, enum gpio_pull pull)
{
    u32 flags;
    u32 reg;

    if (pin >= LUME_GPIO_MAX)
        return -1;

    reg = (pin < 32) ? 0 : 1;

    flags = arm_irq_save();
    GPIO_REG(GPIO_PUD) = (u32)pull;
    for (volatile int i = 0; i < 150; i++)
        ;
    GPIO_REG(reg ? GPIO_PUDCLK1 : GPIO_PUDCLK0) = 1u << (pin % 32);
    for (volatile int i = 0; i < 150; i++)
        ;
    GPIO_REG(GPIO_PUD) = 0;
    GPIO_REG(reg ? GPIO_PUDCLK1 : GPIO_PUDCLK0) = 0;
    arm_irq_restore(flags);
    return 0;
}

void gpio_set_high(u32 pin)
{
    if (pin >= LUME_GPIO_MAX)
        return;
    if (pin < 32)
        GPIO_REG(GPIO_SET0) = 1u << pin;
    else
        GPIO_REG(GPIO_SET1) = 1u << (pin - 32);
}

void gpio_set_low(u32 pin)
{
    if (pin >= LUME_GPIO_MAX)
        return;
    if (pin < 32)
        GPIO_REG(GPIO_CLR0) = 1u << pin;
    else
        GPIO_REG(GPIO_CLR1) = 1u << (pin - 32);
}

void gpio_write(u32 pin, int value)
{
    if (value)
        gpio_set_high(pin);
    else
        gpio_set_low(pin);
}

int gpio_read(u32 pin)
{
    if (pin >= LUME_GPIO_MAX)
        return -1;
    if (pin < 32)
        return (GPIO_REG(GPIO_LEV0) >> pin) & 1;
    return (GPIO_REG(GPIO_LEV1) >> (pin - 32)) & 1;
}
