/*
 * LumeOS PL011 UART driver (BCM2835 ARM Peripherals chapter 13).
 *
 * The Pi Zero W brings UART0 out on the 40-pin header as GPIO14 (TXD) and
 * GPIO15 (RXD); the firmware leaves them muxed to ALT0.  LumeOS drives the
 * PL011 in polled mode for output and with the receive interrupt enabled for
 * console input.
 */
#ifndef LUME_UART_H
#define LUME_UART_H

#include <lume/types.h>

#define LUME_UART_DEFAULT_BAUD 115200

/** Initialise UART0.  Returns 0 on success, negative on failure. */
int uart_init(u32 baud);

/** The baud rate actually programmed (from the firmware-reported clock). */
u32 uart_baud(void);
u32 uart_clock_hz(void);

void uart_putc(char c);
void uart_puts(const char *s);

/** Polled receive; returns -1 when no character is available. */
int uart_getc(void);

/** True when at least one byte is waiting in the receive FIFO. */
int uart_rx_ready(void);

/** Enable/disable the receive interrupt (IRQ 57). */
void uart_enable_rx_irq(int enable);

/** Drain and return one character from the interrupt handler. */
int uart_irq_handler(void);

#endif /* LUME_UART_H */
