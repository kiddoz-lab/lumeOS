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
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/panic.h>
#include <lume/proc.h>
#include <lume/pte.h>
#include <lume/sched.h>
#include <lume/string.h>
#include <lume/time.h>
#include <lume/types.h>

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
 * The intended sequence (docs/roadmap.md) is: find the init program (initramfs
 * or /bin/init on the SD card), load it with the ELF loader, create a user
 * process and enter ARM user mode through the same trap-frame return path the
 * exception vectors use.
 *
 * None of that exists yet: there is no filesystem, no ELF loader and no
 * syscall layer, so this build deliberately drops into the kernel shell and
 * says so, rather than starting a program that cannot work.
 */
static void init_start(void)
{
    pr_notice("init: userspace hand-off is not implemented in this build; "
              "starting the kernel shell instead (docs/roadmap.md)");
    kshell_start();
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

    /* Hand over to userspace when there is an init program; otherwise drop
     * into the kernel shell, which is a bring-up tool rather than a userspace
     * shell (docs/roadmap.md tracks the difference). */
    init_start();

    pr_info("main: entering the idle loop");
    kernel_idle_loop();
}
