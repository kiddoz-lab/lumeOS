/*
 * Host-side test of the parts of the kernel that do not touch hardware:
 * the freestanding printf/string implementations.
 *
 * tests/host/test_kernel_lib.py compiles this file together with
 * kernel/kernel/printf.c and kernel/kernel/string.c for the *host* CPU and
 * runs it.  That gives the format engine and the string library a real test
 * suite without needing an ARM board, while the ARM-only parts are covered by
 * the in-kernel self tests under QEMU (tests/qemu).
 *
 * The file uses the same headers as the kernel, so a mismatch between a header
 * and its implementation shows up here first.
 */
#include <lume/divmod.h>
#include <lume/string.h>
#include <lume/types.h>

/* Both come from kernel/kernel/printf.c; declared here rather than including
 * kernel headers so that the test also works when compiled on the host. */
int ksnprintf(char *buf, u32 size, const char *fmt, ...);

#include <stdio.h>

/*
 * Deliberately *no* <string.h>: every string call in this file must resolve to
 * kernel/kernel/string.c, which is the code under test.  Including the host's
 * <string.h> would both conflict with the kernel prototypes (u32 vs size_t)
 * and silently redirect calls to glibc, making the test meaningless.
 */
static int t_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int failures;
static int checks;

static void expect(const char *what, const char *got, const char *want)
{
    checks++;
    if (t_strcmp(got, want) != 0) {
        failures++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    }
}

static void expect_int(const char *what, long got, long want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: got %ld, want %ld\n", what, got, want);
    }
}

static void check_formatting(void)
{
    char buf[128];

    ksnprintf(buf, sizeof(buf), "%d", -42);
    expect("decimal negative", buf, "-42");
    ksnprintf(buf, sizeof(buf), "%d", 2147483647);
    expect("int max", buf, "2147483647");
    ksnprintf(buf, sizeof(buf), "%u", 4294967295u);
    expect("uint max", buf, "4294967295");
    ksnprintf(buf, sizeof(buf), "%x", 0xdeadbeefu);
    expect("hex", buf, "deadbeef");
    ksnprintf(buf, sizeof(buf), "%X", 0xbeefu);
    expect("HEX", buf, "BEEF");
    ksnprintf(buf, sizeof(buf), "%#x", 0x2au);
    expect("hex prefix", buf, "0x2a");
    ksnprintf(buf, sizeof(buf), "%o", 0777u);
    expect("octal", buf, "777");
    ksnprintf(buf, sizeof(buf), "%b", 6u);
    expect("binary", buf, "110");
    ksnprintf(buf, sizeof(buf), "%c%c", 'h', 'i');
    expect("chars", buf, "hi");
    ksnprintf(buf, sizeof(buf), "%s", "lumeos");
    expect("string", buf, "lumeos");
    ksnprintf(buf, sizeof(buf), "%5d|%-5d|", 42, 42);
    expect("width", buf, "   42|42   |");
    ksnprintf(buf, sizeof(buf), "%05d", 42);
    expect("zero pad", buf, "00042");
    ksnprintf(buf, sizeof(buf), "%+d %+d", 7, -7);
    expect("plus sign", buf, "+7 -7");
    ksnprintf(buf, sizeof(buf), "%.3s", "lumeos");
    expect("precision string", buf, "lum");
    ksnprintf(buf, sizeof(buf), "%.4x", 0xabu);
    expect("precision hex", buf, "00ab");
    ksnprintf(buf, sizeof(buf), "%*d", 6, 99);
    expect("star width", buf, "    99");
    ksnprintf(buf, sizeof(buf), "%lld", -9223372036854775807LL - 1);
    expect("long long min", buf, "-9223372036854775808");
    ksnprintf(buf, sizeof(buf), "%llu", 18446744073709551615ULL);
    expect("ull max", buf, "18446744073709551615");
    ksnprintf(buf, sizeof(buf), "%zu", (u32)1234u);
    expect("size_t", buf, "1234");
    ksnprintf(buf, sizeof(buf), "%hd", (int)(short)-5);
    expect("short", buf, "-5");
    ksnprintf(buf, sizeof(buf), "100%%");
    expect("percent", buf, "100%");
    ksnprintf(buf, sizeof(buf), "%s", (const char *)0);
    expect("null string", buf, "(null)");

    /* Truncation must always NUL-terminate and never write past the buffer. */
    memset(buf, 'x', sizeof(buf));
    int n = ksnprintf(buf, 4, "%s", "abcdefgh");
    expect("truncated", buf, "abc");
    expect_int("truncation count", n, 8);
}

static void check_strings(void)
{
    char buf[32];

    memset(buf, 0xAA, sizeof(buf));
    memset(buf, 0, 5);
    for (int i = 0; i < 5; i++)
        expect_int("memset zero", buf[i], 0);

    strcpy(buf, "lume");
    strcat(buf, "os");
    expect("strcat", buf, "lumeos");
    expect_int("strlen", (long)strlen(buf), 6);

    memcpy(buf, "abcdef", 7);
    expect("memcpy", buf, "abcdef");
    /* memmove only moves the 6 requested bytes; the byte behind them keeps the
     * 0xAA the earlier memset left there, so compare a fixed window. */
    memmove(buf + 1, buf, 6);
    expect_int("memmove overlap", memcmp(buf, "aabcdef", 7), 0);
    expect_int("memmove overlap tail", buf[7], (char)0xAA);
    expect_int("memcmp", memcmp("abc", "abd", 3) < 0, 1);
    {   /* memchr must return the address of the match, not an offset. */
        const char *hay = "lumeos";
        const char *hit = memchr(hay, 'o', 6);
        expect_int("memchr found", (long)(hit - hay), 4);
        expect_int("memchr miss", memchr(hay, 'z', 6) == NULL, 1);
    }

    /* Unaligned targets must work (SCTLR.A is on in the kernel, so the
     * implementation copies byte-wise when alignment differs). */
    char unaligned[16];
    memset(unaligned, 0x11, sizeof(unaligned));
    memcpy(unaligned + 1, "xyz", 4);
    expect("unaligned memcpy", unaligned + 1, "xyz");

    /* strtoul returns the number of characters consumed and hands the value
     * back through the out parameter. */
    {
        u32 v = 0xFFFFFFFFu;

        expect_int("strtoul hex consumed", (long)strtoul("0x1f", &v, 16), 4);
        expect_int("strtoul hex value", (long)v, 31);
        expect_int("strtoul decimal consumed", (long)strtoul("250", &v, 10), 3);
        expect_int("strtoul decimal value", (long)v, 250);
        expect_int("strtoul stops at junk", (long)strtoul("12abc", &v, 10), 2);
        expect_int("strtoul junk value", (long)v, 12);
        expect_int("strtoul no digits", (long)strtoul("xyz", &v, 10), 0);
        expect_int("strtoul auto base", (long)strtoul("0x10", &v, 0), 4);
        expect_int("strtoul auto base value", (long)v, 16);
    }

    char num[16];
    utoa(12345, num, 10);
    expect("utoa", num, "12345");
    itoa(-321, num, 10);
    expect("itoa", num, "-321");
}

/*
 * The division cores behind the ARM EABI helpers.  On the host these are the
 * *same* source lines that run on the Pi (kernel/kernel/divmod.c); what the
 * host cannot check is the register marshalling in
 * kernel/arch/arm/aeabi_div.S, which only the in-kernel self tests under QEMU
 * exercise.  The 32-bit cases still go through the host compiler's own
 * division, so they also prove the cores agree with the C language rules.
 */
static int zero_divisions;

void lume_div_by_zero(const char *width)
{
    (void)width;
    zero_divisions++;
}

static void check_division(void)
{
    u32 uq, ur;
    s32 sq, sr;
    u32 num64[2], den64[2], out64[4];

    lume_udivmod32(1000000u, 7u, &uq, &ur);
    expect_int("udivmod32 quot", (long)uq, 142857);
    expect_int("udivmod32 rem", (long)ur, 1);

    lume_udivmod32(0xFFFFFFFFu, 0x10000u, &uq, &ur);
    expect_int("udivmod32 max quot", (long)uq, 65535);
    expect_int("udivmod32 max rem", (long)ur, 65535);

    lume_idivmod32(-7, 2, &sq, &sr);
    expect_int("idivmod32 quot", sq, -3);
    expect_int("idivmod32 rem", sr, -1);      /* C99: sign of the dividend */

    lume_idivmod32(7, -2, &sq, &sr);
    expect_int("idivmod32 neg den quot", sq, -3);
    expect_int("idivmod32 neg den rem", sr, 1);

    /* 0xC0000000_00000000 / 2 must not lose the top bit. */
    num64[0] = 0x00000000u; num64[1] = 0xC0000000u;
    den64[0] = 2u;          den64[1] = 0u;
    lume_udivmod64(num64, den64, out64);
    expect_int("udivmod64 top-bit quot low", (long)out64[0], 0);
    expect_int("udivmod64 top-bit quot high", (long)out64[1], 0x60000000);
    expect_int("udivmod64 top-bit rem", (long)out64[2], 0);

    /* 10000000000 / 7 = 1428571428 rem 4 (the value the old code divided). */
    num64[0] = 10000000000ull & 0xFFFFFFFFu;
    num64[1] = (u32)(10000000000ull >> 32);
    den64[0] = 7u; den64[1] = 0u;
    lume_udivmod64(num64, den64, out64);
    expect_int("udivmod64 1e10 quot", (long)out64[0], 1428571428);
    expect_int("udivmod64 1e10 rem", (long)out64[2], 4);

    /* -1 / 2 = 0 rem -1, and it must not report a zero divisor. */
    num64[0] = 0xFFFFFFFFu; num64[1] = 0xFFFFFFFFu;
    den64[0] = 2u; den64[1] = 0u;
    lume_ldivmod64(num64, den64, out64);
    expect_int("ldivmod64 -1/2 quot", (long)out64[0], 0);
    expect_int("ldivmod64 -1/2 rem", (long)out64[2], 0xFFFFFFFF);
    expect_int("ldivmod64 -1/2 sign", (long)out64[3], 0xFFFFFFFF);

    /* Division by zero calls the hook and yields zero, not garbage. */
    zero_divisions = 0;
    lume_udivmod32(1u, 0u, &uq, &ur);
    expect_int("udiv32 by zero hook", zero_divisions, 1);
    expect_int("udiv32 by zero quot", (long)uq, 0);

    num64[1] = 0; den64[0] = 0; den64[1] = 0;
    lume_udivmod64(num64, den64, out64);
    expect_int("udiv64 by zero hook", zero_divisions, 2);
    expect_int("udiv64 by zero quot", (long)out64[0], 0);
}

int main(void)
{
    check_formatting();
    check_strings();
    check_division();

    printf("ktest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
