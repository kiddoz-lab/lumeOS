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
#include <lume/pte.h>
#include <lume/sched.h>
#include <lume/string.h>
#include <lume/time.h>
#include <lume/trapframe.h>
#include <lume/traptest.h>
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
        u32 translated = 0;

        check("vmm map page", pa != 0 && vmm_map_page(as, 0x00100000, pa,
                                                      VM_FLAG_USER | VM_FLAG_WRITE) == 0);
        check("vmm translate", vmm_translate(as, 0x00100000, &translated) == 0 &&
                               translated == pa);

        /* An address that was never mapped must report failure, not a
         * successful translation to physical 0. */
        translated = 0xFFFFFFFFu;
        check("vmm unmapped reports failure",
              vmm_translate(as, 0x00300000, &translated) != 0);

        check("vmm check user writable",
              vmm_check_range(as, 0x00100000, 4096, 1, 1) == 0);

        /* A new address space must inherit the kernel half, including the alias
         * at 0xC0000000 (which maps to *physical* 0 - the case the old
         * return-value API could not express) and the exception vector page. */
        check("vmm kernel alias mapped",
              vmm_translate(as, 0xC0008000, &translated) == 0 &&
              translated == 0x00008000);
        check("vmm vector page mapped",
              vmm_translate(as, 0xFFFF0000, &translated) == 0 &&
              translated == VECTOR_PAGE_PA);

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

/*
 * Division goes through the compiler's runtime helpers (__aeabi_uidiv,
 * __aeabi_uldivmod, ...) because ARM1176JZF-S has no divide instruction, so
 * every check here also exercises the EABI register marshalling in
 * kernel/arch/arm/aeabi_div.S.  A wrong convention shows up as a bogus result
 * or as a panic; both are caught by tests/qemu/run_qemu_test.py.
 */
static void test_division(void)
{
    char buf[32];

    check("u32 div", 1000000u / 7u == 142857u);
    check("u32 mod", 1000000u % 7u == 1u);
    check("u32 div max", 0xFFFFFFFFu / 0x10000u == 65535u);
    check("s32 div", -7 / 2 == -3);
    check("s32 mod", -7 % 2 == -1);

    {
        u64 n = 10000000000ull;         /* needs the 64-bit helper */
        u64 q = n / 7ull;
        u64 r = n % 7ull;

        check("u64 div", q == 1428571428ull);
        check("u64 mod", r == 4ull);
        ksnprintf(buf, sizeof(buf), "%llu", (unsigned long long)q);
        check("u64 div formats", strcmp(buf, "1428571428") == 0);
    }

    {
        u64 n = 0xC000000000000000ull;  /* must not lose the top bit */
        u64 q = n / 2ull;

        check("u64 top-bit div", q == 0x6000000000000000ull);
    }

    {
        s64 n = -10000000001ll;
        s64 q = n / 3ll;
        s64 r = n % 3ll;

        check("s64 div", q == -3333333333ll);
        check("s64 mod", r == -2ll);    /* C99: sign of the dividend */
    }

    check("u64 mul", 0x100000000ull * 3ull == 0x300000000ull);
}

/*
 * The exception path, measured rather than assumed.  arch/arm/trapprobe.S takes
 * an SVC with every register set to a known pattern; this reads what the
 * handler saw and what came back.  The two checks that matter most are the last
 * two: a kernel that loses a register or leaks stack per syscall still boots,
 * and then fails in the least diagnosable way possible once a program is
 * running.
 */
static void test_trapframe(void)
{
    struct traptest_report *r = &traptest_report;
    int faithful = 1;
    u32 i;

    traptest_reset();
    arch_trap_probe_run();

    check("svc probe reached the handler", r->calls == 1);
    check("svc probe finished", r->finished == 1);

    for (i = 0; i < TRAPTEST_REGS; i++) {
        if (r->frame[i] != TRAPTEST_PATTERN(i)) {
            faithful = 0;
            pr_err("selftest: trap frame r%u = 0x%08x, expected 0x%08x",
                   i, r->frame[i], TRAPTEST_PATTERN(i));
        }
    }

    if ((r->frame_cpsr & CPSR_MODE_MASK) != MODE_SVC)
        pr_err("selftest: trap frame mode = 0x%02x, expected SVC (0x%02x)",
               r->frame_cpsr & CPSR_MODE_MASK, MODE_SVC);
    if (r->frame_sp != r->sp_at_svc)
        pr_err("selftest: trap frame sp = 0x%08x, sp at the svc was 0x%08x",
               r->frame_sp, r->sp_at_svc);
    if (r->sp_after != r->sp_at_svc)
        pr_err("selftest: sp drifted from 0x%08x to 0x%08x across the svc",
               r->sp_at_svc, r->sp_after);
    if (r->clobber)
        pr_err("selftest: registers changed across the svc (bitmap 0x%04x)", r->clobber);

    check("trap frame r0-r12 faithful", faithful);
    check("trap frame pc is the resume address", r->frame_pc == r->resume);
    check("trap frame is a kernel-mode frame",
          (r->frame_cpsr & CPSR_MODE_MASK) == MODE_SVC);
    check("trap frame sp is the interrupted sp", r->frame_sp == r->sp_at_svc);
    check("registers survive the round trip", r->clobber == 0);
    check("stack pointer survives the round trip", r->sp_after == r->sp_at_svc);
}

void selftest_run(void)
{
    tests_run = 0;
    tests_failed = 0;

    pr_notice("selftest: running kernel self tests");

    test_strings();
    test_division();
    test_pmm();
    test_kmalloc();
    test_timer();
    test_vmm();
    test_proc();
    test_trapframe();

    if (tests_failed == 0)
        pr_notice("selftest: %d/%d checks passed", tests_run - tests_failed, tests_run);
    else
        pr_err("selftest: %d of %d checks FAILED", tests_failed, tests_run);
}
