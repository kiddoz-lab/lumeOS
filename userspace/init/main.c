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

    puts_fd(1, "init: exiting with status 0\n");
    lume_exit(0);
    return 0;   /* not reached */
}
