/*
 * LumeOS PL011 UART driver (UART0 on the Raspberry Pi Zero W).
 *
 * Register offsets and field meanings: BCM2835 ARM Peripherals chapter 13
 * ("Universal Asynchronous Receiver/Transmitter", PL011).  The baud rate
 * divider is computed from the UART clock reported by the VideoCore firmware
 * (mailbox property tag GET_CLOCK_RATE, clock id 2 = UART): the firmware sets
 * this up as a "base clock", typically 3 MHz on a Pi Zero W with the default
 * config.txt, but it is not guaranteed, so it is queried rather than assumed.
 *
 * The character device interface exposes this port as /dev/ttyS0 (see
 * kernel/devfs.c), which is also the system console.
 */
#include <lume/asm.h>
#include <lume/gpio.h>
#include <lume/hw/bcm2835.h>
#include <lume/input.h>
#include <lume/klog.h>
#include <lume/mbox.h>
#include <lume/uart.h>
#include <lume/types.h>

#define UART_REG(off) (*(volatile u32 *)((u32)PERIPHERAL_TO_VIRT(BCM2835_UART0_BASE) + (off)))

/* Set by the driver at init time. */
static u32 uart_clock;
static u32 uart_actual_baud;
static int uart_irq_enabled;

static void uart_gpio_init(void)
{
    /* GPIO14 = TXD (ALT0), GPIO15 = RXD (ALT0).  On the Pi Zero W the
     * firmware normally leaves them muxed this way; setting it again is
     * harmless and makes the driver self-contained. */
    gpio_set_function(GPIO_PIN_UART_TXD, GPIO_FUNC_ALT0);
    gpio_set_function(GPIO_PIN_UART_RXD, GPIO_FUNC_ALT0);
    gpio_set_pull(GPIO_PIN_UART_RXD, GPIO_PULL_UP);
}

static void uart_flush(void)
{
    while (UART_REG(PL011_FR) & PL011_FR_BUSY)
        ;
}

int uart_init(u32 baud)
{
    u32 rate = mbox_get_clock_rate(BCM2835_CLK_UART);
    u32 divisor;

    if (rate == 0) {
        /* Firmware did not answer (or no mailbox): fall back to the rate the
         * Raspberry Pi firmware uses by default, and say so in the log. */
        rate = 3000000u;
        pr_warn("uart: firmware did not report the UART clock, assuming %u Hz", rate);
    }
    uart_clock = rate;

    uart_gpio_init();

    /* Disable the UART while it is reconfigured. */
    UART_REG(PL011_CR) = 0;
    uart_flush();

    /* Clear all pending interrupts (the ICR bits are "write 1 to clear"). */
    UART_REG(PL011_ICR) = 0x7FF;

    /* Integer and fractional baud divisors: divisor = clock / (16 * baud).
     * IBRD is the integer part, FBRD is the fractional part scaled by 64
     * (BCM2835 ARM Peripherals 13.4 "PL011 Registers"). */
    divisor = (rate * 1000u / (16u * baud));  /* scaled by 1000 to keep precision */
    UART_REG(PL011_IBRD) = divisor / 1000u;
    UART_REG(PL011_FBRD) = (((divisor % 1000u) * 64u + 500u) / 1000u) & 0x3Fu;

    /* 8 bits, FIFOs enabled, no parity, one stop bit. */
    UART_REG(PL011_LCRH) = PL011_LCRH_WLEN_8 | PL011_LCRH_FEN;

    /* Enable UART, transmitter and receiver. */
    UART_REG(PL011_CR) = PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE;

    uart_flush();

    /* Report the real baud rate that results from the divisor rounding. */
    {
        u32 ibrd = UART_REG(PL011_IBRD);
        u32 fbrd = UART_REG(PL011_FBRD);
        u32 divisor_x64 = ibrd * 64u + fbrd;
        uart_actual_baud = (divisor_x64 == 0) ? 0 : (rate * 64u) / (16u * divisor_x64);
    }

    /* No interrupts until the input subsystem asks for them. */
    UART_REG(PL011_IMSC) = 0;
    uart_irq_enabled = 0;

    return 0;
}

u32 uart_baud(void)
{
    return uart_actual_baud;
}

u32 uart_clock_hz(void)
{
    return uart_clock;
}

void uart_putc(char c)
{
    /* The PL011 FIFO is 16 bytes deep; wait for space. */
    while (UART_REG(PL011_FR) & PL011_FR_TXFF)
        ;
    UART_REG(PL011_DR) = (u32)(u8)c;
}

void uart_puts(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s);
    }
}

int uart_rx_ready(void)
{
    return !(UART_REG(PL011_FR) & PL011_FR_RXFE);
}

int uart_getc(void)
{
    if (!uart_rx_ready())
        return -1;
    return (int)(UART_REG(PL011_DR) & 0xFFu);
}

void uart_enable_rx_irq(int enable)
{
    u32 flags = arm_irq_save();

    if (enable) {
        UART_REG(PL011_ICR) = PL011_ICR_RXIC | PL011_ICR_RTIC;
        UART_REG(PL011_IMSC) = PL011_IMSC_RXIM | PL011_IMSC_RTIM;
        uart_irq_enabled = 1;
    } else {
        UART_REG(PL011_IMSC) = 0;
        uart_irq_enabled = 0;
    }
    arm_irq_restore(flags);
}

int uart_irq_handler(void)
{
    u32 mis = UART_REG(PL011_MIS);
    int count = 0;

    if (!(mis & (PL011_IMSC_RXIM | PL011_IMSC_RTIM)))
        return 0;

    /* Drain the FIFO; the input layer consumes the characters. */
    while (!(UART_REG(PL011_FR) & PL011_FR_RXFE)) {
        int c = (int)(UART_REG(PL011_DR) & 0xFF);
        input_serial_feed((char)c);
        count++;
        if (count > 64)
            break; /* stay bounded if the FIFO never empties */
    }
    UART_REG(PL011_ICR) = PL011_ICR_RXIC | PL011_ICR_RTIC;
    return count;
}

int uart_irq_is_enabled(void)
{
    return uart_irq_enabled;
}
