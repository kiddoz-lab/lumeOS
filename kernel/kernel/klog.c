/*
 * LumeOS kernel log.
 *
 * printk-style logging with:
 *   - a level filter (pr_* macros in include/lume/klog.h);
 *   - up to four output consoles (the PL011 serial port always, a framebuffer
 *     console once one exists);
 *   - a fixed 16 KiB ring buffer that userspace can read through /dev/kmsg
 *     (or the LUME_SYS_KLOG syscall), which is how boot messages survive into
 *     a shell session.
 *
 * The implementation is allocation-free and works from the very first
 * instruction of kernel_main, which is what makes early bring-up debuggable on
 * a board whose only output device is a serial line.
 */
#include <lume/asm.h>
#include <lume/config.h>
#include <lume/klog.h>
#include <lume/string.h>
#include <lume/time.h>
#include <lume/types.h>

#define MAX_CONSOLES 4

struct console {
    const char *name;
    void (*putc)(char c);
    int active;
};

static struct console consoles[MAX_CONSOLES];
static int console_count;
static int log_level = KLOG_LEVEL_INFO;

static char ring[KLOG_BUFFER_SIZE];
static u32 ring_write;
static u32 ring_count;
static int log_ready;

/* Set by time.c once the monotonic clock is usable; before that, log lines
 * print "[boot]" instead of a timestamp. */
extern u64 time_monotonic_ms(void);

static void log_emit(const char *msg, u32 len)
{
    int n = console_count;

    for (int i = 0; i < n; i++) {
        if (consoles[i].active && consoles[i].putc)
            for (u32 j = 0; j < len; j++)
                consoles[i].putc(msg[j]);
    }
}

static void ring_push(const char *msg, u32 len)
{
    for (u32 i = 0; i < len; i++) {
        ring[ring_write] = msg[i];
        ring_write = (ring_write + 1) % KLOG_BUFFER_SIZE;
        if (ring_count < KLOG_BUFFER_SIZE)
            ring_count++;
    }
}

void klog_init(void)
{
    console_count = 0;
    ring_write = 0;
    ring_count = 0;
    memset(consoles, 0, sizeof(consoles));
    memset(ring, 0, sizeof(ring));
    log_ready = 1;
}

void klog_set_level(int level)
{
    if (level < KLOG_LEVEL_EMERG)
        level = KLOG_LEVEL_EMERG;
    if (level > KLOG_LEVEL_DEBUG)
        level = KLOG_LEVEL_DEBUG;
    log_level = level;
}

int klog_get_level(void)
{
    return log_level;
}

int console_register(const char *name, void (*putc)(char c))
{
    if (!putc)
        return -1;
    if (console_count >= MAX_CONSOLES)
        return -1;

    consoles[console_count].name = name;
    consoles[console_count].putc = putc;
    consoles[console_count].active = 1;
    console_count++;
    return 0;
}

void kputc(char c)
{
    if (!log_ready) {
        /* Before klog_init() there may still be a console that wants the
         * character (panic_early path). */
        for (int i = 0; i < console_count; i++)
            if (consoles[i].active && consoles[i].putc)
                consoles[i].putc(c);
        return;
    }

    log_emit(&c, 1);
    ring_push(&c, 1);
}

void kputs(const char *s)
{
    u32 len;

    if (!s)
        return;
    len = strlen(s);

    if (!log_ready) {
        for (int i = 0; i < console_count; i++)
            if (consoles[i].active && consoles[i].putc)
                for (u32 j = 0; j < len; j++)
                    consoles[i].putc(s[j]);
        return;
    }

    log_emit(s, len);
    ring_push(s, len);
}

void klog_write(int level, const char *fmt, ...)
{
    char msg[256];
    char line[320];
    va_list ap;
    u32 flags;
    u32 n = 0;

    if (level > log_level)
        return;

    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    flags = arm_irq_save();

    if (time_is_running()) {
        u64 ms = time_monotonic_ms();
        n += (u32)ksnprintf(line + n, sizeof(line) - n, "[%5llu.%03llu] ",
                            (unsigned long long)(ms / 1000),
                            (unsigned long long)(ms % 1000));
    } else {
        n += (u32)ksnprintf(line + n, sizeof(line) - n, "[boot] ");
    }

    const char *prefix = "";
    switch (level) {
    case KLOG_LEVEL_EMERG:
    case KLOG_LEVEL_ALERT:
    case KLOG_LEVEL_CRIT:
        prefix = "PANIC: ";
        break;
    case KLOG_LEVEL_ERR:
        prefix = "ERROR: ";
        break;
    case KLOG_LEVEL_WARNING:
        prefix = "WARNING: ";
        break;
    case KLOG_LEVEL_DEBUG:
        prefix = "debug: ";
        break;
    default:
        break;
    }

    n += (u32)ksnprintf(line + n, sizeof(line) - n, "%s%s\n", prefix, msg);
    kputs(line);

    arm_irq_restore(flags);
}

u32 klog_read(char *dst, u32 max)
{
    u32 flags;
    u32 total;
    u32 start;

    if (!dst || max == 0)
        return 0;

    flags = arm_irq_save();
    total = (max < ring_count) ? max : ring_count;

    /* Read the oldest bytes first, leaving the newest in the buffer unless the
     * caller asked for more than we have.  This matches the "read drains"
     * semantics documented for /dev/kmsg. */
    start = (ring_write + KLOG_BUFFER_SIZE - ring_count) % KLOG_BUFFER_SIZE;
    for (u32 i = 0; i < total; i++)
        dst[i] = ring[(start + i) % KLOG_BUFFER_SIZE];
    ring_count -= total;
    arm_irq_restore(flags);
    return total;
}

u32 klog_pending(void)
{
    u32 count;

    u32 flags = arm_irq_save();
    count = ring_count;
    arm_irq_restore(flags);
    return count;
}
