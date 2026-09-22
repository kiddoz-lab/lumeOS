/*
 * LumeOS freestanding C string/memory routines.
 *
 * All word-at-a-time copies check alignment explicitly: LumeOS enables
 * SCTLR.A (alignment checking) so an unaligned word access would fault.
 */
#include <lume/string.h>
#include <lume/types.h>

void *memset(void *dst, int c, u32 n)
{
    u8 *d = dst;
    u8 v = (u8)c;

    while (n && !IS_ALIGNED((uintptr_t_lume)d, 4)) {
        *d++ = v;
        n--;
    }
    if (n >= 4) {
        u32 w = v | (v << 8) | (v << 16) | (v << 24);
        u32 *dw = (u32 *)d;
        while (n >= 4) {
            *dw++ = w;
            n -= 4;
        }
        d = (u8 *)dw;
    }
    while (n--)
        *d++ = v;
    return dst;
}

u32 memzero(void *dst, u32 n)
{
    memset(dst, 0, n);
    return n;
}

void *memcpy(void *dst, const void *src, u32 n)
{
    u8 *d = dst;
    const u8 *s = src;
    void *ret = dst;

    if (n == 0)
        return ret;

    /* If both sides are equally misaligned, alignment-fix them first. */
    if ((((uintptr_t_lume)d ^ (uintptr_t_lume)s) & 3u) == 0) {
        while (n && !IS_ALIGNED((uintptr_t_lume)d, 4)) {
            *d++ = *s++;
            n--;
        }
        while (n >= 4) {
            *(u32 *)(void *)d = *(const u32 *)(const void *)s;
            d += 4;
            s += 4;
            n -= 4;
        }
    }
    while (n--)
        *d++ = *s++;
    return ret;
}

void *memmove(void *dst, const void *src, u32 n)
{
    u8 *d = dst;
    const u8 *s = src;

    if (d == s || n == 0)
        return dst;
    if (d < s || d >= s + n)
        return memcpy(dst, src, n);

    /* Overlapping and dst > src: copy backwards. */
    d += n;
    s += n;
    while (n--)
        *--d = *--s;
    return dst;
}

int memcmp(const void *a, const void *b, u32 n)
{
    const u8 *x = a, *y = b;

    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

void *memchr(const void *s, int c, u32 n)
{
    const u8 *p = s;

    while (n--) {
        if (*p == (u8)c)
            return (void *)(uintptr_t_lume)p;
        p++;
    }
    return NULL;
}

u32 strlen(const char *s)
{
    u32 n = 0;

    if (!s)
        return 0;
    while (s[n])
        n++;
    return n;
}

u32 strnlen(const char *s, u32 max)
{
    u32 n = 0;

    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

int strncmp(const char *a, const char *b, u32 n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(u8)*a - (int)(u8)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *ret = dst;

    while ((*dst++ = *src++))
        ;
    return ret;
}

char *strncpy(char *dst, const char *src, u32 n)
{
    char *ret = dst;
    u32 i = 0;

    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return ret;
}

char *strcat(char *dst, const char *src)
{
    char *ret = dst;

    while (*dst)
        dst++;
    while ((*dst++ = *src++))
        ;
    return ret;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++)
        if (*s == (char)c)
            return (char *)(uintptr_t_lume)s;
    return (c == 0) ? (char *)(uintptr_t_lume)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;

    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            break;
    }
    return (char *)(uintptr_t_lume)last;
}

char *strstr(const char *haystack, const char *needle)
{
    u32 nlen = strlen(needle);

    if (nlen == 0)
        return (char *)(uintptr_t_lume)haystack;
    for (; *haystack; haystack++)
        if (strncmp(haystack, needle, nlen) == 0)
            return (char *)(uintptr_t_lume)haystack;
    return NULL;
}

static int digit_value(char c, u32 base)
{
    int v;

    if (c >= '0' && c <= '9')
        v = c - '0';
    else if (c >= 'a' && c <= 'z')
        v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z')
        v = c - 'A' + 10;
    else
        return -1;
    return (v < (int)base) ? v : -1;
}

u32 strtoul(const char *s, u32 *out, u32 base)
{
    u32 value = 0;
    u32 consumed = 0;
    int neg = 0;

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
            consumed += 2;
        } else if (s[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        /* The "0x" prefix is consumed but contributes no digits; count it so
         * that the return value means the same thing here as in the base == 0
         * branch above and matches what callers (and strtol_signed) expect. */
        s += 2;
        consumed += 2;
    }

    for (;; s++) {
        int d = digit_value(*s, base);
        if (d < 0)
            break;
        value = value * base + (u32)d;
        consumed++;
    }
    if (out)
        *out = neg ? (u32)(-(s32)value) : value;
    return consumed;
}

int strtol_signed(const char *s, s32 *out, u32 base)
{
    const char *p = s;
    int neg = 0;
    u32 value;
    u32 consumed;

    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') p++;

    consumed = strtoul(p, &value, base);
    if (consumed == 0)
        return 0;
    *out = neg ? -(s32)value : (s32)value;
    return (int)(consumed + (u32)(p - s));
}

char *utoa(u32 value, char *buf, u32 base)
{
    char tmp[34];
    u32 n = 0;

    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return buf;
    }
    while (value) {
        u32 d = value % base;
        tmp[n++] = (d < 10) ? (char)('0' + d) : (char)('a' + d - 10);
        value /= base;
    }
    for (u32 i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
    return buf;
}

char *itoa(s32 value, char *buf, u32 base)
{
    if (value < 0) {
        buf[0] = '-';
        utoa((u32)(-(value + 1)) + 1, buf + 1, base);
        return buf;
    }
    return utoa((u32)value, buf, base);
}
