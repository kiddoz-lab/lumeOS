/*
 * LumeOS serial console input.
 *
 * The character stream from the PL011 is turned into a cooked input queue
 * here: CR/LF normalisation, backspace handling, and a small ring buffer that
 * both the kernel shell and (later) /dev/ttyS0 read from.  The same queue is
 * what a userspace shell sees, so the line discipline lives in the kernel
 * exactly as it does on a real Unix terminal.
 *
 * Raw events (key presses, touch samples) are the natural extension point for
 * the USB keyboard and touchscreen drivers on the roadmap; they will push
 * into this core rather than inventing a second queue.
 */
#include <lume/asm.h>
#include <lume/input.h>
#include <lume/klog.h>
#include <lume/sched.h>
#include <lume/types.h>
#include <lume/uart.h>

#define INPUT_QUEUE_SIZE 256

static char queue[INPUT_QUEUE_SIZE];
static u32 q_head;      /* next byte to read */
static u32 q_count;
static int input_ready;

/* A wait queue so readers can block instead of spinning (single global one is
 * enough while there is a single console). */
static struct wait_queue input_wq;
static int echo_enabled = 1;

void input_init(void)
{
    q_head = 0;
    q_count = 0;
    input_ready = 1;
    wait_queue_init(&input_wq, "console");
    pr_info("input: console input queue ready (%u bytes)", INPUT_QUEUE_SIZE);
}

void input_start_polling(void)
{
    /* Drain anything the firmware left in the FIFO (it may have been used as
     * an early console before LumeOS took over). */
    input_poll();
}

static void queue_push(char c)
{
    u32 flags = arm_irq_save();

    if (q_count < INPUT_QUEUE_SIZE) {
        queue[(q_head + q_count) % INPUT_QUEUE_SIZE] = c;
        q_count++;
        /* Wake a blocked reader. */
        if (input_wq.head)
            wait_queue_wake_one(&input_wq);
    }
    arm_irq_restore(flags);
}

void input_serial_feed(char c)
{
    /* Normalise line endings: a serial terminal sends CR, a program may send
     * LF; both mean "end of line" to the console. */
    if (c == '\r')
        c = '\n';

    /* Backspace: delete the previous character unless the line is empty.
     * This is a simple version of the canonical mode kernel tty does; the
     * full termios machinery is future work (docs/roadmap.md). */
    if (c == 0x7F || c == '\b') {
        if (q_count > 0) {
            u32 last = (q_head + q_count - 1) % INPUT_QUEUE_SIZE;
            if (queue[last] != '\n') {
                q_count--;
                if (echo_enabled) {
                    kputc('\b');
                    kputc(' ');
                    kputc('\b');
                }
            }
        }
        return;
    }

    if (echo_enabled && (c == '\n' || (c >= 0x20 && c < 0x7F)))
        kputc(c);

    queue_push(c);

    /* A future revision delivers SIGINT on ^C; until signals and processes are
     * further along the character is simply passed through, which is stated
     * here rather than silently ignored. */
}

int input_console_read(char *buf, u32 len, int blocking)
{
    u32 flags;
    u32 copied = 0;

    if (!input_ready)
        return -1;

    for (;;) {
        flags = arm_irq_save();
        if (q_count > 0) {
            while (copied < len && q_count > 0) {
                buf[copied++] = queue[q_head];
                q_head = (q_head + 1) % INPUT_QUEUE_SIZE;
                q_count--;
            }
            arm_irq_restore(flags);
            return (int)copied;
        }
        arm_irq_restore(flags);

        if (!blocking)
            return 0;

        /* Block until the interrupt handler pushes something. */
        wait_event(&input_wq, 0);
    }
}

u32 input_console_available(void)
{
    u32 count;

    u32 flags = arm_irq_save();
    count = q_count;
    arm_irq_restore(flags);
    return count;
}

void input_poll(void)
{
    int c;
    int guard = 0;

    while ((c = uart_getc()) >= 0) {
        input_serial_feed((char)c);
        if (++guard > 256)
            break;
    }
}

void input_set_echo(int on)
{
    echo_enabled = on;
}
