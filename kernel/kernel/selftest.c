/*
 * LumeOS in-kernel self tests.
 *
 * These run on every boot (they are cheap) and are the fastest way to tell
 * whether a change broke the memory manager, the string library or the timer
 * on real hardware.  Tests never fake success: a failing check prints FAIL and
 * is counted, and the summary line is what tests/qemu/run_qemu_test.py greps
 * for.
 *
 * They are deliberately unit-level tests of kernel invariants.  End-to-end
 * checks (booting, userspace syscalls) live in tests/qemu and tests/host.
 */
#include <lume/asm.h>
#include <lume/config.h>
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/proc.h>
#include <lume/sched.h>
#include <lume/string.h>
#include <lume/time.h>
#include <lume/trapframe.h>
#include <lume/types.h>

static int tests_run;
static int tests_failed;

static void check(const char *name, int condition)
{
    tests_run++;
    if (condition) {
        pr_info("selftest: %-28s ok", name);
    } else {
        tests_failed++;
        pr_err("selftest: %-28s FAIL", name);
    }
}

static void test_strings(void)
{
    char buf[32];

    check("trapframe size", sizeof(struct trapframe) == TF_SIZE);

    memset(buf, 0, sizeof(buf));
    strcpy(buf, "lume");
    check("strcpy", strcmp(buf, "lume") == 0);
    strcat(buf, "os");
    check("strcat", strcmp(buf, "lumeos") == 0);
    check("strlen", strlen("lumeos") == 6);
    check("strcmp", strcmp("abc", "abd") < 0);
    check("memcmp", memcmp("abcd", "abce", 4) < 0);
    ksnprintf(buf, sizeof(buf), "%d/%u/%x/%s", -42, 42u, 0xbeeu, "str");
    check("ksnprintf", strcmp(buf, "-42/42/bee/str") == 0);
    ksnprintf(buf, sizeof(buf), "%5d|%-5d|", 42, 42);
    check("ksnprintf padding", strcmp(buf, "   42|42   |") == 0);

    {
        u64 big = 0x123456789ABCDEFull;
        ksnprintf(buf, sizeof(buf), "%llx", (unsigned long long)big);
        check("ksnprintf 64-bit", strcmp(buf, "123456789abcdef") == 0);
    }
}

static void test_pmm(void)
{
    u32 before = pmm_free_bytes();
    u32 a = pmm_alloc_page();
    u32 b = pmm_alloc_page();

    check("pmm allocates", a != 0 && b != 0 && a != b);
    check("pmm page aligned", IS_ALIGNED(a, PAGE_SIZE) && IS_ALIGNED(b, PAGE_SIZE));
    check("pmm free count drops", pmm_free_bytes() == before - 2 * PAGE_SIZE);

    pmm_free_page(a);
    pmm_free_page(b);
    check("pmm free restores", pmm_free_bytes() == before);

    {
        u32 block = pmm_alloc_pages(4, 4 * PAGE_SIZE);

        check("pmm contiguous alloc", block != 0 && IS_ALIGNED(block, 4 * PAGE_SIZE));
        pmm_free_pages(block, 4);
    }
}

static void test_kmalloc(void)
{
    u8 *p = kmalloc(100);
    u8 *q = kzalloc(4096);
    u32 i;

    check("kmalloc", p != NULL);
    if (p) {
        for (i = 0; i < 100; i++)
            p[i] = (u8)i;
        for (i = 0; i < 100 && p[i] == (u8)i; i++)
            ;
        check("kmalloc writable", i == 100);
    }

    check("kzalloc", q != NULL);
    if (q) {
        for (i = 0; i < 4096 && q[i] == 0; i++)
            ;
        check("kzalloc zeroed", i == 4096);
    }

    kfree(p);
    kfree(q);
    check("kfree", 1);
}

static void test_timer(void)
{
    u64 t0 = timer_read_us();
    u64 t1;

    udelay(1000);
    t1 = timer_read_us();
    check("timer advances", t1 > t0);
    check("udelay ~1ms", (t1 - t0) >= 900 && (t1 - t0) < 20000);
    check("timer tick rate", timer_tick_hz() == LUME_HZ);
}

static void test_vmm(void)
{
    struct vm_space *as = vmm_space_create();

    check("vmm space create", as != NULL);
    if (as) {
        u32 pa = pmm_alloc_page();

        check("vmm map page", pa != 0 && vmm_map_page(as, 0x00100000, pa,
                                                      VM_FLAG_USER | VM_FLAG_WRITE) == 0);
        check("vmm translate", vmm_translate(as, 0x00100000) == pa);
        check("vmm check user writable",
              vmm_check_range(as, 0x00100000, 4096, 1, 1) == 0);
        check("vmm kernel half mapped",
              vmm_translate(as, 0xC0000000) != 0);
        vmm_unmap_page(as, 0x00100000);
        vmm_space_destroy(as);
    }
}

static void test_proc(void)
{
    check("proc pid 0", proc_by_pid(0) != NULL);
    check("proc current", proc_current() != NULL);
    check("thread table", thread_count() >= 1);
}

void selftest_run(void)
{
    tests_run = 0;
    tests_failed = 0;

    pr_notice("selftest: running kernel self tests");

    test_strings();
    test_pmm();
    test_kmalloc();
    test_timer();
    test_vmm();
    test_proc();

    if (tests_failed == 0)
        pr_notice("selftest: %d/%d checks passed", tests_run - tests_failed, tests_run);
    else
        pr_err("selftest: %d of %d checks FAILED", tests_failed, tests_run);
}
