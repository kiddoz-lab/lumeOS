/*
 * LumeOS kernel shell.
 *
 * This is a *kernel* tool, not userspace: it exists so that a board with no
 * userspace yet (or with a broken init) can still be inspected and so that
 * automated tests have something to talk to over the serial line.  It runs as
 * an ordinary kernel thread and blocks on the console input queue, which also
 * makes it a live test of the scheduler, the interrupt path and the input
 * queue at once.
 *
 * The userspace shell is a separate program (/bin/lsh, docs/userspace.md);
 * LumeOS deliberately does not name its own shell "bash" or pretend to be one.
 */
#include <lume/asm.h>
#include <lume/config.h>
#include <lume/errno.h>
#include <lume/input.h>
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/panic.h>
#include <lume/sched.h>
#include <lume/string.h>
#include <lume/time.h>
#include <lume/types.h>

void arch_reboot(void);
void arch_poweroff(void);
void input_set_echo(int on);
u32 irq_total_count(void);
u32 irq_spurious_count(void);
u64 timer_ticks(void);
u64 timer_polled_ticks(void);
void proc_dump(void (*emit)(const char *line, void *arg), void *arg);

#define LINE_MAX 128

static void emit_line(const char *line, void *arg)
{
    (void)arg;
    kputs(line);
    kputc('\n');
}

static void cmd_help(void)
{
    kputs("kernel shell commands:\n"
          "  help            this list\n"
          "  mem             physical and heap memory statistics\n"
          "  ps              processes and threads\n"
          "  time            uptime, wall clock state, timer tick counters\n"
          "  irq             interrupt counters\n"
          "  echo <text>     print the text back\n"
          "  reboot          reset the SoC through the watchdog\n"
          "  halt            stop the CPU (power still on)\n"
          "  version         kernel identity strings\n"
          "This is the LumeOS kernel shell; the userspace shell is /bin/lsh.\n");
}

static void cmd_mem(void)
{
    pr_notice("pmm: total %u KiB, free %u KiB, used %u KiB, %u pages",
              pmm_total_bytes() / 1024, pmm_free_bytes() / 1024,
              pmm_used_bytes() / 1024, pmm_page_count());
    pr_notice("kmalloc: total %u KiB, used %u KiB",
              kmalloc_total_bytes() / 1024, kmalloc_used_bytes() / 1024);
}

static void cmd_time(void)
{
    u64 us = time_monotonic_us();

    pr_notice("time: uptime %llu.%03llu s (monotonic), wall clock %s",
              (unsigned long long)(us / 1000000),
              (unsigned long long)((us / 1000) % 1000),
              time_is_set() ? "set" : "unset (no RTC on this board)");
    pr_notice("timer: %llu ticks total, %llu serviced by the fallback poll",
              (unsigned long long)timer_ticks(),
              (unsigned long long)timer_polled_ticks());
}

static void cmd_irq(void)
{
    pr_notice("irq: %u handled, %u spurious",
              irq_total_count(), irq_spurious_count());
}

static void cmd_ps(void)
{
    pr_notice("  pid  ppid  pgid    sid  state    fds");
    proc_dump(emit_line, NULL);
    pr_notice("threads: %u", thread_count());
}

static void cmd_version(void)
{
    pr_notice("%s %s, %s, %s", LUME_NAME, LUME_VERSION, LUME_ARCH, LUME_MACHINE);
    pr_notice("build: commit %s", LUME_BUILD_COMMIT);
}

static void run_command(char *line)
{
    char *arg = strchr(line, ' ');

    if (arg) {
        *arg = '\0';
        arg++;
        while (*arg == ' ')
            arg++;
    }

    if (line[0] == '\0') {
        return;
    } else if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        cmd_help();
    } else if (strcmp(line, "mem") == 0 || strcmp(line, "free") == 0) {
        cmd_mem();
    } else if (strcmp(line, "ps") == 0) {
        cmd_ps();
    } else if (strcmp(line, "time") == 0 || strcmp(line, "uptime") == 0) {
        cmd_time();
    } else if (strcmp(line, "irq") == 0) {
        cmd_irq();
    } else if (strcmp(line, "version") == 0 || strcmp(line, "uname") == 0) {
        cmd_version();
    } else if (strcmp(line, "echo") == 0) {
        kputs(arg ? arg : "");
        kputc('\n');
    } else if (strcmp(line, "reboot") == 0) {
        arch_reboot();
    } else if (strcmp(line, "halt") == 0) {
        arch_poweroff();
    } else {
        kputs("unknown command: ");
        kputs(line);
        kputs(" (try 'help')\n");
    }
}

static void kshell_thread(void *arg)
{
    char line[LINE_MAX];
    u32 used = 0;

    (void)arg;

    kputs("\nLumeOS kernel shell. Type 'help' for the command list.\n");
    kputs("lume> ");

    for (;;) {
        char c;

        if (input_console_read(&c, 1, 1) != 1)
            continue;

        if (c == '\n') {
            if (used < LINE_MAX - 1)
                line[used] = '\0';
            else
                line[LINE_MAX - 1] = '\0';
            run_command(line);
            used = 0;
            kputs("lume> ");
            continue;
        }

        if (used < LINE_MAX - 1)
            line[used++] = c;
    }
}

void kshell_start(void)
{
    struct thread *t = thread_create("kshell", kshell_thread, NULL);

    if (!t)
        pr_err("kshell: cannot start the kernel shell");
    else
        pr_info("kshell: kernel shell thread tid %u started", t->tid);
}
