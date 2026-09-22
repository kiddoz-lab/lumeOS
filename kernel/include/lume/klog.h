/*
 * LumeOS kernel logging.
 *
 * printk() writes to every registered console (serial first, framebuffer
 * console once it exists) and appends to a fixed-size in-memory ring buffer
 * that userspace can read through /dev/kmsg or the native syscall
 * LUME_SYS_KLOG_READ.  No dynamic allocation and no locking beyond an
 * interrupt-disable critical section: logging works from the earliest boot
 * code and from interrupt context.
 */
#ifndef LUME_KLOG_H
#define LUME_KLOG_H

#include <lume/types.h>
#include <lume/compiler.h>
#include <lume/stdarg.h>

#define KLOG_LEVEL_EMERG   0
#define KLOG_LEVEL_ALERT   1
#define KLOG_LEVEL_CRIT    2
#define KLOG_LEVEL_ERR     3
#define KLOG_LEVEL_WARNING 4
#define KLOG_LEVEL_NOTICE  5
#define KLOG_LEVEL_INFO    6
#define KLOG_LEVEL_DEBUG   7

#define KLOG_BUFFER_SIZE 16384

#ifdef CONFIG_LUME_VERBOSE
#define LUME_LOG_DEFAULT_LEVEL KLOG_LEVEL_DEBUG
#else
#define LUME_LOG_DEFAULT_LEVEL KLOG_LEVEL_INFO
#endif

void klog_init(void);
void klog_set_level(int level);
int  klog_get_level(void);

/** Format and emit a log record to all consoles and the ring buffer. */
void klog_write(int level, const char *fmt, ...) __printf(2, 3);

/** Write a raw string (no formatting, no prefix) to every console. */
void kputs(const char *s);
void kputc(char c);

/** Read from the ring buffer; returns bytes copied, 0 when empty. */
u32 klog_read(char *dst, u32 max);

/** Number of bytes currently held in the ring buffer. */
u32 klog_pending(void);

/** Register a console sink.  The callback receives one character. */
int console_register(const char *name, void (*putc)(char c));

#define pr_emerg(...)   klog_write(KLOG_LEVEL_EMERG, __VA_ARGS__)
#define pr_alert(...)   klog_write(KLOG_LEVEL_ALERT, __VA_ARGS__)
#define pr_crit(...)    klog_write(KLOG_LEVEL_CRIT, __VA_ARGS__)
#define pr_err(...)     klog_write(KLOG_LEVEL_ERR, __VA_ARGS__)
#define pr_warn(...)    klog_write(KLOG_LEVEL_WARNING, __VA_ARGS__)
#define pr_notice(...)  klog_write(KLOG_LEVEL_NOTICE, __VA_ARGS__)
#define pr_info(...)    klog_write(KLOG_LEVEL_INFO, __VA_ARGS__)
#define pr_debug(...)   klog_write(KLOG_LEVEL_DEBUG, __VA_ARGS__)

/* Formatted output into a caller-provided buffer.  Returns the number of
 * characters that would have been written (like C99 snprintf). */
int kvsnprintf(char *buf, u32 size, const char *fmt, va_list ap);
int ksnprintf(char *buf, u32 size, const char *fmt, ...) __printf(3, 4);

#endif /* LUME_KLOG_H */
