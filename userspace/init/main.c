/*
 * LumeOS init - the first userspace program.
 *
 * This is an ordinary ARM Linux binary in one important sense: it is a static
 * 32-bit ARM ELF that talks to the kernel with the Linux ARM EABI syscall
 * instruction.  It is *not* an ordinary Linux program yet, and it does not
 * pretend to be: it is freestanding (no libc, no dynamic linker, no
 * relocations) and it uses only the syscalls this kernel implements.  Every
 * line it prints is produced by write(2) on file descriptor 1, so seeing it on
 * the console means the whole path worked - user mode, the syscall
 * instruction, the kernel's dispatch, the console device and the return to
 * user mode.
 *
 * docs/userspace.md describes what a program like this may assume, what it may
 * not, and how the real /bin/init with a filesystem behind it will replace the
 * embedded blob.
 */
#include <lume/auxv.h>
#include <lume/syscall.h>

/* Freestanding build: no headers, no printf, no malloc. */
typedef unsigned int u32;
typedef unsigned long u32len;

static u32len len_of(const char *s)
{
    u32len n = 0;

    while (s[n])
        n++;
    return n;
}

static void puts_fd(int fd, const char *s)
{
    lume_write(fd, s, len_of(s));
}

static void put_hex(int fd, u32 value)
{
    static const char digits[] = "0123456789abcdef";
    char buf[8];

    puts_fd(fd, "0x");
    for (int i = 0; i < 8; i++) {
        buf[i] = digits[(value >> ((7 - i) * 4)) & 0xF];
    }
    lume_write(fd, buf, 8);
}

static void put_hex8(int fd, u32 value)
{
    static const char digits[] = "0123456789abcdef";

    puts_fd(fd, "0x");
    for (int i = 1; i >= 0; i--)
        lume_write(fd, &digits[(value >> (i * 4)) & 0xF], 1);
}

static void put_dec(int fd, u32 value)
{
    char buf[11];
    int i = 0;

    if (value == 0) {
        lume_write(fd, "0", 1);
        return;
    }
    while (value && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (value % 10));
        value /= 10;
    }
    /* The digits came out backwards; write them in the right order without
     * needing another buffer (write(2) takes a length, not a string). */
    while (i-- > 0)
        lume_write(fd, &buf[i], 1);
}


/* ------------------------------------------------------------------ */
/* The auxiliary vector                                                */
/* ------------------------------------------------------------------ */

/*
 * A program finds the auxiliary vector by walking past argv and envp, which is
 * what a C library's startup code does and what libc's __libc_start_main is
 * handed.  There is no system call for it: it is part of the initial stack, and
 * this walk *is* the check that the stack the kernel built has the shape the
 * ABI promises - a missing NULL terminator would send this loop off the end.
 */
static const u32 *find_auxv(int argc, char **argv)
{
    const u32 *w = (const u32 *)argv;
    u32 i;

    w += argc + 1;              /* argc word, argc pointers, the NULL */
    for (i = 0; w[i]; i++)      /* envp: strings until its NULL */
        ;
    return w + i + 1;
}

static u32 auxv_get(const u32 *auxv, u32 type, u32 fallback)
{
    for (u32 i = 0; auxv[i] != LUME_AT_NULL; i += 2)
        if (auxv[i] == type)
            return auxv[i + 1];
    return fallback;
}

static int bytes_are_zero(const unsigned char *p, u32len n)
{
    for (u32len i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

int main(int argc, char **argv)
{
    long pid = lume_getpid();

    puts_fd(1, "init: hello from user mode\n");
    puts_fd(1, "init: pid ");
    put_dec(1, (u32)pid);
    puts_fd(1, ", argc ");
    put_dec(1, (u32)argc);
    puts_fd(1, "\n");

    /* argv is a real pointer into the stack the kernel built for us.  Reading
     * it proves the stack layout arrived intact, and printing it proves it is
     * readable from user mode. */
    if (argc > 0 && argv && argv[0]) {
        puts_fd(1, "init: argv[0] is \"");
        puts_fd(1, argv[0]);
        puts_fd(1, "\"\n");
    } else {
        puts_fd(1, "init: argv is empty\n");
    }

    /* stderr goes to the same console device through the same syscall. */
    puts_fd(2, "init: this line went to file descriptor 2\n");

    /*
     * The auxiliary vector.  Every check here is about an interface between two
     * separately compiled pieces - this program and the kernel - and each one
     * would be a silent corruption rather than a crash if it were wrong:
     *
     *   AT_PAGESZ  the kernel's page size.  A program that maps memory with
     *              mmap(2) (not implemented yet) does arithmetic with this.
     *   AT_ENTRY   where the program started.  Compared against the address of
     *              _start, so the number must be the address of our own code.
     *   AT_PHDR    the program headers.  A C library reads them to find
     *              PT_GNU_STACK and to decide whether the process is PIE; here
     *              the header is read and the ELF magic and e_entry inside it
     *              are checked, which is the only way a program can prove the
     *              pointer really addresses its own image.
     *   AT_HWCAP   what the CPU may be asked to do.  Checked to *lack* VFP:
     *              this kernel does not save floating-point state across a
     *              context switch, so a program that used VFP would have its
     *              registers clobbered - see docs/userspace.md.
     *   AT_RANDOM  16 bytes for a stack canary.  Checked to be present, to be
     *              readable, and to not be 16 zeroes (the failure mode if the
     *              kernel never filled it in).
     *   AT_CLKTCK  the frequency times(2) counts in.
     */
    {
        extern char _start[];               /* from userspace/lib/crt0.S */
        const u32 *auxv = find_auxv(argc, argv);
        u32 pagesz = auxv_get(auxv, LUME_AT_PAGESZ, 0);
        u32 entry = auxv_get(auxv, LUME_AT_ENTRY, 0);
        u32 phdr = auxv_get(auxv, LUME_AT_PHDR, 0);
        u32 phnum = auxv_get(auxv, LUME_AT_PHNUM, 0);
        u32 hwcap = auxv_get(auxv, LUME_AT_HWCAP, 0);
        u32 clktck = auxv_get(auxv, LUME_AT_CLKTCK, 0);
        u32 random_va = auxv_get(auxv, LUME_AT_RANDOM, 0);
        int ok = 1;

        puts_fd(1, "init: auxv at ");
        put_hex(1, (u32)auxv);
        puts_fd(1, ": AT_PAGESZ ");
        put_dec(1, pagesz);
        puts_fd(1, ", AT_ENTRY ");
        put_hex(1, entry);
        puts_fd(1, ", AT_PHDR ");
        put_hex(1, phdr);
        puts_fd(1, ", AT_PHNUM ");
        put_dec(1, phnum);
        puts_fd(1, ", AT_HWCAP ");
        put_hex(1, hwcap);
        puts_fd(1, ", AT_CLKTCK ");
        put_dec(1, clktck);
        puts_fd(1, "\n");

        ok &= (pagesz == 4096);
        ok &= (entry == (u32)_start);
        ok &= (clktck == 100);
        ok &= ((hwcap & LUME_HWCAP_VFP) == 0);   /* soft-float build only */

        /* AT_PHDR must address this program's own ELF header.  Read it and
         * check the magic and that the entry inside it is the entry we were
         * started at - two independent facts, both from the image itself. */
        if (phdr && phnum) {
            const unsigned char *h = (const unsigned char *)phdr;
            u32 header_entry;

            ok &= (h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F');
            header_entry = (u32)h[24] | ((u32)h[25] << 8) |
                           ((u32)h[26] << 16) | ((u32)h[27] << 24);
            ok &= (header_entry == entry);
            puts_fd(1, "init: auxv AT_PHDR reads back as ELF, e_entry ");
            put_hex(1, header_entry);
            puts_fd(1, "\n");
        } else {
            ok = 0;
        }

        /* AT_RANDOM: readable, and not the zeroes an unfilled stack would give. */
        if (random_va) {
            int nonzero = !bytes_are_zero((const unsigned char *)random_va, 16);

            ok &= nonzero;
            puts_fd(1, "init: auxv AT_RANDOM 16 bytes, seed byte ");
            put_hex8(1, *(const unsigned char *)random_va);
            puts_fd(1, nonzero ? " (not all zero)\n" : " (ALL ZERO)\n");
        } else {
            ok = 0;
        }

        if (!ok) {
            puts_fd(2, "init: auxv check FAILED\n");
            lume_exit(1);
        }
        puts_fd(1, "init: auxv verified\n");
    }

    puts_fd(1, "init: exiting with status 0\n");
    lume_exit(0);
    return 0;   /* not reached */
}
