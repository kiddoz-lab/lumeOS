/*
 * The console character device.
 *
 * The kernel talks to the PL011 directly for its own log output; userspace
 * must not, so this is the device node those file descriptors point at.  It is
 * a real fs_node with a real ops table, not a special case in the syscall
 * layer, because that is how the rest of the system is built: write(1, ...) on
 * a console and write(1, ...) on a file will follow the same path once a
 * filesystem exists.
 *
 * What it does not have yet, and cannot pretend to have: line discipline
 * settings (the input core holds one global cooked mode), terminal ioctls, and
 * separate read/write sides for more than one console.  docs/userspace.md
 * records those as gaps rather than features.
 */
#include <lume/errno.h>
#include <lume/fcntl.h>
#include <lume/fs.h>
#include <lume/input.h>
#include <lume/klog.h>
#include <lume/string.h>
#include <lume/types.h>
#include <lume/uart.h>

static int console_open(struct fs_node *node, u32 flags);
static int console_close(struct fs_node *node);
static int console_read(struct fs_node *node, u32 offset, void *buf, u32 len);
static int console_write(struct fs_node *node, u32 offset, const void *buf, u32 len);
static int console_ioctl(struct fs_node *node, u32 request, u32 arg);

static struct fs_ops console_ops = {
    .read = console_read,
    .write = console_write,
    .open = console_open,
    .close = console_close,
    .ioctl = console_ioctl,
};

static struct fs_node console_node = {
    .name = "console",
    .type = FS_TYPE_CHAR,
    .mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP,
    .dev = 0x0500,   /* the classic Linux console device number */
    .ops = &console_ops,
    .nlink = 1,
};

struct fs_node *console_device(void)
{
    return &console_node;
}

static int console_open(struct fs_node *node, u32 flags)
{
    (void)node;
    (void)flags;
    return 0;
}

static int console_close(struct fs_node *node)
{
    (void)node;
    return 0;
}

static int console_write(struct fs_node *node, u32 offset, const void *buf, u32 len)
{
    const char *bytes = (const char *)buf;

    (void)node;
    (void)offset;

    for (u32 i = 0; i < len; i++) {
        /* Newline translation, like a terminal in cooked mode: the Pi's
         * console is a serial line, and a program that writes "\n" expects the
         * cursor to return to column 0 as well. */
        if (bytes[i] == '\n')
            uart_putc('\r');
        uart_putc(bytes[i]);
    }
    return (int)len;
}

static int console_read(struct fs_node *node, u32 offset, void *buf, u32 len)
{
    (void)node;
    (void)offset;

    if (len == 0)
        return 0;
    /* Blocking read with the console line discipline in front of it; when
     * there is no input the calling thread sleeps in the input core's wait
     * queue rather than spinning. */
    return input_console_read((char *)buf, len, 1);
}

static int console_ioctl(struct fs_node *node, u32 request, u32 arg)
{
    (void)node;
    (void)request;
    (void)arg;
    /* No terminal driver yet: report it as "not a terminal ioctl" instead of
     * returning success for something that did nothing. */
    return -ENOTTY;
}
