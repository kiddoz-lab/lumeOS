/*
 * LumeOS input infrastructure.
 *
 * The input core is deliberately small and split into two layers:
 *
 *   - the *driver* layer pushes raw events (currently only serial console
 *     characters) into the core;
 *   - the *console line discipline* turns those characters into a cooked
 *     stream that /dev/console and /dev/ttyS0 read from, including echo,
 *     backspace handling and (once signals exist) ^C.
 *
 * A future USB HID keyboard driver attaches at the driver layer and feeds
 * key events to the same core; see docs/roadmap.md for the exact remaining
 * work.  Nothing here pretends a USB stack exists yet.
 */
#ifndef LUME_INPUT_H
#define LUME_INPUT_H

#include <lume/types.h>

void input_init(void);

/** Push one character from the serial driver into the line discipline. */
void input_serial_feed(char c);

/** Read cooked console input.  With blocking != 0 the caller sleeps until at
 *  least one byte is available.  Returns the number of bytes copied. */
int input_console_read(char *buf, u32 len, int blocking);

/** Number of bytes currently waiting in the console input queue. */
u32 input_console_available(void);

/** Poll the serial port for characters (used before interrupts are up). */
void input_poll(void);

/** Drain anything already buffered by the firmware and mark input ready. */
void input_start_polling(void);

/** Enable/disable local echo of typed characters. */
void input_set_echo(int on);

#endif /* LUME_INPUT_H */
