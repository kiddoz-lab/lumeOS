/*
 * LumeOS formatted output.
 *
 * A small, freestanding implementation of the printf family supporting the
 * conversions the kernel and its userspace need:
 *
 *   %d %i %u %x %X %o %b %p %c %s %%
 *   length modifiers: l, ll, z, h, hh
 *   flags: '-' (left justify), '0' (zero pad), '+' , ' ' and '#'
 *   field width and precision, including '*' widths
 *
 * Deliberately no floating point: the ARM1176JZF-S in the Raspberry Pi Zero W
 * has no VFP unit and the kernel never needs it.
 */
#include <lume/klog.h>
#include <lume/stdarg.h>
#include <lume/string.h>
#include <lume/types.h>

struct fmt_sink {
    char *buf;
    u32 size;
    u32 written; /* characters that would have been written */
};

static void sink_putc(struct fmt_sink *s, char c)
{
    if (s->written + 1 < s->size)
        s->buf[s->written] = c;
    s->written++;
}

static void sink_write(struct fmt_sink *s, const char *str, u32 len)
{
    for (u32 i = 0; i < len; i++)
        sink_putc(s, str[i]);
}

static void sink_pad(struct fmt_sink *s, char c, int count)
{
    while (count-- > 0)
        sink_putc(s, c);
}

/* Very small random-access sink used while assembling one conversion. */
struct numbuf {
    char data[32];
    u32 len;
    char prefix[4];
    u32 prefix_len;
};

static void numbuf_add(struct numbuf *nb, char c)
{
    if (nb->len < sizeof(nb->data))
        nb->data[nb->len++] = c;
}

static void numbuf_uint(struct numbuf *nb, u64 value, u32 base, int upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    nb->len = 0;
    if (value == 0) {
        numbuf_add(nb, '0');
        return;
    }
    char tmp[32];
    u32 n = 0;
    while (value) {
        tmp[n++] = digits[value % base];
        value /= base;
    }
    for (u32 i = 0; i < n; i++)
        numbuf_add(nb, tmp[n - 1 - i]);
}

static void numbuf_emit(struct fmt_sink *s, struct numbuf *nb,
                        int width, int left, int zero, int prec)
{
    u32 total = nb->len + nb->prefix_len;
    u32 zeros = 0;

    if (prec >= 0 && (u32)prec > nb->len)
        zeros = (u32)prec - nb->len;

    u32 padded = total + zeros;
    int pad = (width > (int)padded) ? width - (int)padded : 0;

    if (!left) {
        if (zero && prec < 0)
            sink_pad(s, '0', pad);
        else
            sink_pad(s, ' ', pad);
    }
    sink_write(s, nb->prefix, nb->prefix_len);
    sink_pad(s, '0', (int)zeros);
    sink_write(s, nb->data, nb->len);
    if (left)
        sink_pad(s, ' ', pad);
}

int kvsnprintf(char *buf, u32 size, const char *fmt, va_list ap)
{
    struct fmt_sink s = { buf, size ? size : 1, 0 };

    if (!buf || size == 0)
        return 0;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            sink_putc(&s, *p);
            continue;
        }
        p++;

        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else if (*p == '+') plus = 1;
            else if (*p == ' ') space = 1;
            else if (*p == '#') alt = 1;
            else break;
        }

        int width = 0;
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left = 1; width = -width; }
            p++;
        } else {
            while (*p >= '0' && *p <= '9')
                width = width * 10 + (*p++ - '0');
        }

        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') { prec = va_arg(ap, int); p++; }
            else while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0');
        }

        int is_long = 0, is_longlong = 0, is_size = 0;
        if (*p == 'l') {
            is_long = 1; p++;
            if (*p == 'l') { is_longlong = 1; p++; }
        } else if (*p == 'z' || *p == 't') {
            is_size = 1; p++;
        } else if (*p == 'h') {
            p++;
            if (*p == 'h') p++; /* integer promotion makes these no-ops */
        }

        struct numbuf nb = { .data = {0}, .len = 0, .prefix = {0}, .prefix_len = 0 };

        switch (*p) {
        case '%':
            sink_putc(&s, '%');
            continue;
        case 'c': {
            char c = (char)va_arg(ap, int);
            int pad = width > 1 ? width - 1 : 0;
            if (!left) sink_pad(&s, ' ', pad);
            sink_putc(&s, c);
            if (left) sink_pad(&s, ' ', pad);
            continue;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            u32 len = 0;
            if (!str) str = "(null)";
            while (str[len] && (prec < 0 || (int)len < prec)) len++;
            int pad = width > (int)len ? width - (int)len : 0;
            if (!left) sink_pad(&s, ' ', pad);
            sink_write(&s, str, len);
            if (left) sink_pad(&s, ' ', pad);
            continue;
        }
        case 'p': {
            u32 v = (u32)(uintptr_t_lume)va_arg(ap, void *);
            if (v == 0) {
                sink_write(&s, "(null)", 6);
                continue;
            }
            nb.prefix[0] = '0';
            nb.prefix[1] = 'x';
            nb.prefix_len = 2;
            numbuf_uint(&nb, v, 16, 0);
            /* pointers are printed with a minimum of 8 digits */
            if (nb.len < 8)
                prec = 8;
            numbuf_emit(&s, &nb, width, left, 0, prec);
            continue;
        }
        case 'd':
        case 'i': {
            s64 v;
            if (is_longlong) v = va_arg(ap, s64);
            else if (is_long || is_size) v = va_arg(ap, long);
            else v = va_arg(ap, int);
            int neg = (v < 0);
            u64 uv = neg ? (u64)(-(v + 1)) + 1 : (u64)v;
            numbuf_uint(&nb, uv, 10, 0);
            if (neg) nb.prefix[nb.prefix_len++] = '-';
            else if (plus) nb.prefix[nb.prefix_len++] = '+';
            else if (space) nb.prefix[nb.prefix_len++] = ' ';
            break;
        }
        case 'u': {
            u64 v;
            if (is_longlong) v = va_arg(ap, u64);
            else if (is_long || is_size) v = va_arg(ap, unsigned long);
            else v = va_arg(ap, unsigned int);
            numbuf_uint(&nb, v, 10, 0);
            break;
        }
        case 'x':
        case 'X': {
            u64 v;
            if (is_longlong) v = va_arg(ap, u64);
            else if (is_long || is_size) v = va_arg(ap, unsigned long);
            else v = va_arg(ap, unsigned int);
            if (alt && v) {
                nb.prefix[0] = '0';
                nb.prefix[1] = (*p == 'X') ? 'X' : 'x';
                nb.prefix_len = 2;
            }
            numbuf_uint(&nb, v, 16, *p == 'X');
            break;
        }
        case 'o': {
            u64 v;
            if (is_longlong) v = va_arg(ap, u64);
            else v = va_arg(ap, unsigned int);
            numbuf_uint(&nb, v, 8, 0);
            break;
        }
        case 'b': {
            u64 v;
            if (is_longlong) v = va_arg(ap, u64);
            else if (is_long) v = va_arg(ap, unsigned long);
            else v = va_arg(ap, unsigned int);
            numbuf_uint(&nb, v, 2, 0);
            break;
        }
        default:
            sink_putc(&s, '%');
            if (*p) sink_putc(&s, *p);
            continue;
        }

        numbuf_emit(&s, &nb, width, left, zero, prec);
    }

    buf[(s.written < size) ? s.written : (size - 1)] = '\0';
    return (int)s.written;
}

int ksnprintf(char *buf, u32 size, const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}
