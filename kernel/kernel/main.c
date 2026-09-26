/*
 * LumeOS kernel entry point.
 *
 * boot.S has already built the boot translation tables, enabled the MMU and
 * the caches, installed the per-mode stacks and the high vector page.  From
 * here on everything runs at the kernel virtual alias 0xC0000000 + physical
 * with a working C runtime.
 *
 * The boot order below is deliberate, and each step documents why it must
 * come where it does.  docs/boot.md describes the same sequence from the
 * operator's point of view.
 */
#include <lume/asm.h>
#include <lume/config.h>
#include <lume/console.h>
#include <lume/elf.h>
#include <lume/fd.h>
#include <lume/fs.h>
#include <lume/auxv.h>
#include <lume/fcntl.h>
#include <lume/init.h>
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/panic.h>
#include <lume/proc.h>
#include <lume/pte.h>
#include <lume/sched.h>
#include <lume/string.h>
#include <lume/time.h>
#include <lume/types.h>
#include <lume/ustack.h>

/* Symbols provided by the linker script. */
extern char _kernel_image_end_pa[];

/* Architecture hooks (arch/arm/startup.c). */
void arch_early_init(u32 fdt_pa, u32 load_addr);
void arch_devices_init(void);
void arch_console_enable_input(void);

/* Memory detection (arch/arm/memdetect.c). */
void memdetect_set_fdt(u32 fdt_pa);
void memdetect_init(void);
u32 memdetect_ram_base(void);
u32 memdetect_ram_end(void);
u32 memdetect_fdt(void);

/* Subsystems that live in their own translation units. */
void selftest_run(void);
void kshell_start(void);
void timer_register_irq(void);
u32 irq_total_count(void);

/* Physical page pinned by the boot stub for the high vector page. */

/* The Raspberry Pi firmware keeps its ATAGS/device tree, the boot command
 * line and its own state in low memory.  Reserving the first 8 MiB costs
 * nothing on a 512 MiB board and removes a whole class of "the firmware
 * scribbled over our page" bugs; the first 32 MiB of RAM stay usable. */
#define FIRMWARE_RESERVED_LOW 0x00800000u

void kernel_main(u32 fdt_pa, u32 load_addr);

/*
 * Userspace hand-off.
 *
 * There is no filesystem yet, so the first program is embedded in the kernel
 * image (tools/embed_user.py) - but it is loaded, mapped and entered exactly
 * the way a /bin/init from a card would be: parse the ELF, map each PT_LOAD
 * segment into a fresh address space, build the initial stack with argc/argv,
 * and let the scheduler return to user mode through the same trap-frame path
 * every later exception uses.
 *
 * The three lines this prints are the evidence that the whole chain worked:
 * "entering user mode" comes from the kernel, the program's own output comes
 * from a write(2) issued in user mode, and the exit status comes from the
 * kernel reaping the process it started.
 */
static struct process *init_proc;

/* A kernel thread that behaves like a shell's parent: it waits for init to
 * exit and reports the status.  Doing it with a real wait4-style wait (rather
 * than polling the process table) means the mechanism a shell will use is the
 * one being exercised. */
static void init_watchdog(void *arg)
{
    struct process *init = (struct process *)arg;
    struct process *parent = init->parent;
    u32 status = 0;
    int pid = proc_wait(parent, (s32)init->pid, 0, &status);

    if (pid < 0) {
        pr_err("init: wait for pid %u failed (%d)", init->pid, pid);
        return;
    }
    /* Linux wait(2) status encoding: normal exit is (code & 0xFF) << 8. */
    if ((status & 0x7F) == 0)
        pr_notice("init: pid %u exited with status %u (exit code %u)",
                  init->pid, status, (status >> 8) & 0xFF);
    else
        pr_notice("init: pid %u was killed by signal %u", init->pid, status & 0x7F);
}

/*
 * Build the initial user stack: the strings the program can see, then argv,
 * then argc, exactly as the Linux ABI describes process entry
 * (see docs/userspace.md section 2).  Returns the stack pointer to start with,
 * or 0 on failure.
 *
 * Every store goes through copy_to_user_as() with `p`'s address space, not
 * through copy_to_user(): the process has not been scheduled yet, so the MMU is
 * still on the kernel's tables and "the current space" is the wrong space to
 * validate against.  That distinction is the reason those functions take a
 * space argument at all.
 */
/*
 * The first program's starting state: argv, envp and the auxiliary vector, all
 * built by kernel/kernel/ustack.c.  The kernel supplies the facts it owns - the
 * entry point, the program headers, the credentials - and nothing else:
 *
 *   - the environment is empty.  There is no init script, no PATH to set and
 *     nothing to export, so inventing variables here would be inventing policy.
 *     A program started by a future shell will get the shell's environment.
 *   - argv[0] is the path it was started with; AT_EXECFN says the same thing to
 *     a program that has already rearranged argv.
 */
static u32 build_user_stack(struct process *p, struct elf_image *image,
                            const char **argv, const char **envp)
{
    struct user_startup start;
    struct user_stack_info info;

    memset(&start, 0, sizeof(start));
    start.path = LUME_DEFAULT_INIT;
    start.argv = argv;
    start.envp = envp;
    start.entry = image->entry;
    start.phdr = image->phdr;
    start.phnum = image->phnum;
    start.phent = image->phent;
    start.uid = p->uid;
    start.euid = p->euid;
    start.gid = p->gid;
    start.egid = p->egid;

    if (!user_stack_build_ex(p->as, &start, &info))
        return 0;

    pr_info("init: stack at 0x%08x..0x%08x (%u pages, %u bytes: argc/argv/envp "
            "+ %u auxv entries)", info.sp, LUME_USER_STACK_TOP, info.pages,
            info.bytes, info.auxv_entries);
    pr_info("init: auxv AT_PAGESZ %u, AT_ENTRY 0x%08x, AT_PHDR 0x%08x, "
            "AT_PHNUM %u, AT_HWCAP 0x%08x, AT_CLKTCK %u",
            PAGE_SIZE, image->entry, image->phdr, image->phnum,
            LUME_HWCAP_ARMv6KZ, LUME_AT_CLKTCK_VALUE);
    return info.sp;
}

struct process *init_start(struct process *launcher)
{
    struct elf_image image;
    struct process *p;
    u32 sp;
    const char *argv[2];
    const char *envp[1];
    u32 flags;

    pr_notice("init: loading the embedded %u-byte init image (entry 0x%08x, "
              "sha256 %s)", lume_init_elf_size, lume_init_elf_entry,
              lume_init_elf_sha256);

    /* The process and its thread are created first (a process owns its address
     * space) and the entry point is fixed up once the image has been parsed;
     * interrupts stay off across the whole sequence so the new thread cannot
     * be scheduled before there is anything to run. */
    flags = arm_irq_save();
    p = proc_create("init", 0, 0, 0, launcher);
    if (!p) {
        arm_irq_restore(flags);
        pr_err("init: cannot create the init process");
        return NULL;
    }

    if (elf_load(p->as, lume_init_elf, lume_init_elf_size, &image) < 0) {
        pr_err("init: the embedded image did not load");
        proc_discard(p, launcher);
        arm_irq_restore(flags);
        return NULL;
    }
    proc_set_brk_base(p, image.image_end);

    argv[0] = LUME_DEFAULT_INIT;
    argv[1] = NULL;
    envp[0] = NULL;
    sp = build_user_stack(p, &image, argv, envp);
    if (!sp) {
        pr_err("init: cannot build the initial stack");
        proc_discard(p, launcher);
        arm_irq_restore(flags);
        return NULL;
    }

    /* Three standard descriptors on the console device, like any process a
     * shell would start. */
    for (int fd = 0; fd < 3; fd++)
        fd_open_node(p, console_device(), fd == 0 ? O_RDONLY : O_WRONLY);

    arch_thread_set_user_entry(p->thread, image.entry, sp, 0);
    arm_irq_restore(flags);

    pr_notice("init: entering user mode at 0x%08x on stack 0x%08x",
              image.entry, sp);
    return p;
}

static u32 kernel_image_end_pa;
static u32 kernel_load_pa;

static void print_banner(void)
{
    pr_notice("LumeOS %s (%s) -- an operating system for the Raspberry Pi Zero W",
              LUME_VERSION, LUME_ARCH);
    pr_info("kernel: entered at virtual 0x%08x, loaded at physical 0x%08x, "
            "image ends at 0x%08x",
            (u32)(uintptr_t_lume)&kernel_main, kernel_load_pa, kernel_image_end_pa);
}

static void memory_init(void)
{
    u32 ram_base = memdetect_ram_base();
    u32 ram_end = memdetect_ram_end();
    u32 fdt = memdetect_fdt();

    pmm_init(ram_base, ram_end);

    /* Reserve everything that is already in use before the first allocation:
     * firmware data, this kernel image, the boot page table (inside the low
     * 8 MiB) and the high-vector page. */
    pmm_reserve(ram_base, FIRMWARE_RESERVED_LOW - ram_base);
    pmm_reserve(kernel_load_pa, kernel_image_end_pa - kernel_load_pa);
    pmm_reserve(VECTOR_PAGE_PA, PAGE_SIZE);
    if (fdt >= ram_base && fdt < ram_end)
        pmm_reserve(PAGE_ALIGN_DOWN(fdt), 0x10000);

    vmm_init(ram_base, ram_end);
    kmalloc_init();

    pr_info("memory: %u MiB RAM at 0x%08x, %u KiB free after reservations",
            (ram_end - ram_base) / (1024 * 1024), ram_base, pmm_free_bytes() / 1024);
}

static void kernel_idle_loop(void)
{
    for (;;) {
        /* Interrupts are enabled here on purpose: this is the only place a
         * kernel-mode IRQ is taken, and it is what wakes the CPU from WFI.
         * The fallback poll keeps the scheduler tick alive even if the
         * interrupt routing differs from the documentation; see
         * arch/arm/timer.c for the reasoning. */
        arm_irq_enable();
        arm_wfi();
        timer_poll_fallback();

        if (sched_need_resched())
            schedule();
    }
}

void kernel_main(u32 fdt_pa, u32 load_addr)
{
    kernel_image_end_pa = (u32)(uintptr_t_lume)_kernel_image_end_pa;
    kernel_load_pa = load_addr;

    /* Logging first: every later step may need to report progress or fail. */
    klog_init();
    klog_set_level(KLOG_LEVEL_INFO);

    arch_early_init(fdt_pa, load_addr);
    memdetect_init();
    print_banner();

    memory_init();

    /* Thread/process tables and the scheduler.  sched_init() creates the idle
     * thread, which needs the page allocator and the heap. */
    thread_init_subsystem();
    proc_init();
    sched_init();

    /* Interrupt controller and system timer; after this the tick runs. */
    arch_devices_init();
    time_init();
    arch_console_enable_input();

    /* Kernel self tests, before userspace depends on any of this. */
    selftest_run();

    pr_notice("LumeOS: boot complete, %u KiB free, %llu timer ticks",
              pmm_free_bytes() / 1024, (unsigned long long)timer_ticks());

    /* Hand over to the first user process.  If that fails the kernel says so
     * and drops into its own shell, which is a bring-up tool rather than a
     * userspace shell (docs/roadmap.md tracks the difference). */
    init_proc = init_start(proc_current());
    if (init_proc) {
        if (!thread_create("init-watchdog", init_watchdog, init_proc)) {
            pr_warn("main: cannot start the init watchdog; the exit status of "
                    "pid %u will not be reported", init_proc->pid);
        }
    } else {
        pr_notice("init: no user process was started; starting the kernel shell");
        kshell_start();
    }

    pr_info("main: entering the idle loop");
    kernel_idle_loop();
}
