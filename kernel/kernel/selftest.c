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
#include <lume/auxv.h>
#include <lume/config.h>
#include <lume/elf.h>
#include <lume/init.h>
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/proc.h>
#include <lume/pte.h>
#include <lume/random.h>
#include <lume/sched.h>
#include <lume/string.h>
#include <lume/time.h>
#include <lume/trapframe.h>
#include <lume/traptest.h>
#include <lume/types.h>
#include <lume/ustack.h>

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

/*
 * The ELF loader's validation half, tested against the image the kernel will
 * actually run - which is the point: a validator that accepts everything, or
 * rejects the one image it is given, is useless, and both failure modes are
 * invisible until the machine is in front of you.
 *
 * The mutations below are made on a copy of the first 64 bytes (the header)
 * because the original is in .rodata and must stay pristine.
 */
static void test_elf(void)
{
    const struct elf32_ehdr *ehdr = NULL;
    const char *reason = NULL;
    u8 header[64];

    check("elf: embedded init validates",
          elf_validate(lume_init_elf, lume_init_elf_size, &ehdr, &reason) == 0);
    check("elf: entry matches the symbol table",
          ehdr && ehdr->e_entry == lume_init_elf_entry);
    check("elf: image is a static ARM executable",
          ehdr && ehdr->e_type == ELF_ET_EXEC && ehdr->e_machine == ELF_EM_ARM);

    if (lume_init_elf_size > sizeof(header)) {
        memcpy(header, lume_init_elf, sizeof(header));

        header[16] = (u8)((ELF_ET_DYN >> 0) & 0xFF);  /* e_type = ET_DYN */
        header[17] = (u8)((ELF_ET_DYN >> 8) & 0xFF);
        check("elf: PIE is refused",
              elf_validate(header, sizeof(header), NULL, &reason) < 0);

        memcpy(header, lume_init_elf, sizeof(header));
        header[18] = 0x3E;   /* e_machine = EM_X86_64 */
        header[19] = 0x00;
        check("elf: a foreign architecture is refused",
              elf_validate(header, sizeof(header), NULL, &reason) < 0);

        memcpy(header, lume_init_elf, sizeof(header));
        header[0] = 'X';     /* not an ELF file at all */
        check("elf: a non-ELF image is refused",
              elf_validate(header, sizeof(header), NULL, &reason) < 0);

        check("elf: a truncated image is refused",
              elf_validate(lume_init_elf, 32, NULL, &reason) < 0);
    }
}


/*
 * The initial user stack: the ABI contract the first program is handed.
 *
 * init checks the stack it is actually running on.  This checks the builder, in
 * the kernel, with inputs init does not have (two arguments, a non-empty
 * environment) and with the assertions a program cannot make about itself: that
 * the NULL terminators are there, that the auxiliary vector ends in AT_NULL,
 * that the 16 random bytes in user memory are the ones the generator produced,
 * and that everything the pointers name is readable in the target address space.
 *
 * The vector is walked one pair at a time rather than read as a block on
 * purpose: a bounded read of a region whose size depends on the layout would be
 * a read past the end of the stack, which is the kind of test that fails for
 * its own reasons.
 */
static void test_ustack(void)
{
    const char *argv[] = { "/bin/init", "--selftest", NULL };
    const char *envp[] = { "LUME=1", NULL };
    struct user_startup start;
    struct user_stack_info info;
    struct vm_space *as = vmm_space_create();
    u32 tables[8];
    u32 expect[4];
    u32 sp, auxv_sp;
    u8 expect_bytes[16];

    check("ustack space create", as != NULL);
    if (!as)
        return;

    memset(&start, 0, sizeof(start));
    start.path = "/bin/init";
    start.argv = argv;
    start.envp = envp;
    start.entry = 0x00010000;
    start.phdr = 0x00010034;
    start.phnum = 3;
    start.phent = 32;
    start.uid = start.euid = 0;
    start.gid = start.egid = 0;

    /* Seed the generator twice with the same value and record what the stream
     * produces: the build consumes exactly four words of it (16 bytes), so the
     * bytes that reach user memory are known in advance and can be compared. */
    lume_random_seed(0x12345678u);
    for (u32 i = 0; i < 4; i++)
        expect[i] = lume_random_u32();
    memcpy(expect_bytes, expect, sizeof(expect_bytes));
    lume_random_seed(0x12345678u);

    sp = user_stack_build_ex(as, &start, &info);
    check("ustack builds", sp != 0);
    if (!sp) {
        vmm_space_destroy(as);
        return;
    }

    check("ustack sp is 16-byte aligned", IS_ALIGNED(sp, 16));
    check("ustack maps at least the minimum",
          info.pages >= LUME_USER_STACK_MIN_PAGES);
    check("ustack sp lies inside its own pages",
          sp >= LUME_USER_STACK_TOP - info.pages * PAGE_SIZE);
    check("ustack bytes used", info.bytes > 0 &&
          info.bytes <= info.pages * PAGE_SIZE);
    check("ustack reports one auxv entry per pair", info.auxv_entries ==
          user_auxv_count());

    /* The tables, read exactly the way a program walks them. */
    if (copy_from_user_as(as, tables, (void *)sp, sizeof(tables)) != 0) {
        check("ustack tables readable", 0);
        vmm_space_destroy(as);
        return;
    }
    check("ustack tables readable", 1);
    check("ustack argc", tables[0] == 2);
    check("ustack argv pointers", tables[1] != 0 && tables[2] != 0);
    check("ustack argv NULL terminator", tables[3] == 0);
    check("ustack envp pointer", tables[4] != 0);
    check("ustack envp NULL terminator", tables[5] == 0);

    /* The strings those pointers name must be readable and correct. */
    {
        char buf[16];

        if (copy_from_user_as(as, buf, (void *)tables[1], 11) == 0 &&
            strcmp(buf, "/bin/init") == 0)
            check("ustack argv[0] string", 1);
        else
            check("ustack argv[0] string", 0);
        if (copy_from_user_as(as, buf, (void *)tables[4], 8) == 0 &&
            strcmp(buf, "LUME=1") == 0)
            check("ustack envp[0] string", 1);
        else
            check("ustack envp[0] string", 0);
    }

    /* The auxiliary vector: pair by pair, until AT_NULL. */
    auxv_sp = sp + 4 * (1 + 3 + 2);      /* argc + argv + NULL + envp + NULL */
    {
        u32 entries = 0, pagesz = 0, entry = 0, phdr = 0, phnum = 0, hwcap = 0;
        u32 clktck = 0, random_va = 0, execfn_va = 0, terminated = 0;
        u32 pair[2];
        u32 i;

        for (i = 0; i <= user_auxv_count(); i++) {
            if (copy_from_user_as(as, pair, (void *)(auxv_sp + 8 * i), 8) < 0)
                break;
            if (pair[0] == LUME_AT_NULL) {
                terminated = 1;
                break;
            }
            entries++;
            switch (pair[0]) {
            case LUME_AT_PAGESZ: pagesz = pair[1]; break;
            case LUME_AT_ENTRY:  entry = pair[1]; break;
            case LUME_AT_PHDR:   phdr = pair[1]; break;
            case LUME_AT_PHNUM:  phnum = pair[1]; break;
            case LUME_AT_HWCAP:  hwcap = pair[1]; break;
            case LUME_AT_CLKTCK: clktck = pair[1]; break;
            case LUME_AT_RANDOM: random_va = pair[1]; break;
            case LUME_AT_EXECFN: execfn_va = pair[1]; break;
            default: break;
            }
        }

        check("ustack auxv readable and terminated", terminated == 1);
        check("ustack auxv pair count matches the emitter", entries == user_auxv_count());
        check("ustack AT_PAGESZ", pagesz == PAGE_SIZE);
        check("ustack AT_ENTRY", entry == start.entry);
        check("ustack AT_PHDR", phdr == start.phdr);
        check("ustack AT_PHNUM", phnum == start.phnum);
        check("ustack AT_CLKTCK", clktck == LUME_AT_CLKTCK_VALUE);
        check("ustack AT_HWCAP advertises no floating point",
              (hwcap & LUME_HWCAP_FP_MASK) == 0);
        check("ustack AT_HWCAP advertises the ARMv6 core",
              (hwcap & LUME_HWCAP_ARMv6KZ) == LUME_HWCAP_ARMv6KZ);

        /* AT_RANDOM: the bytes must be exactly the generator's output, which
         * proves they were written and that the pointer addresses them - an
         * all-zero stack would fail this, and so would a truncated one. */
        if (random_va) {
            u8 got[16];

            if (copy_from_user_as(as, got, (void *)random_va, 16) == 0 &&
                memcmp(got, expect_bytes, 16) == 0)
                check("ustack AT_RANDOM is the generator's 16 bytes", 1);
            else
                check("ustack AT_RANDOM is the generator's 16 bytes", 0);
        } else {
            check("ustack AT_RANDOM is the generator's 16 bytes", 0);
        }

        if (execfn_va) {
            char buf[16];

            if (copy_from_user_as(as, buf, (void *)execfn_va, 11) == 0 &&
                strcmp(buf, "/bin/init") == 0)
                check("ustack AT_EXECFN string", 1);
            else
                check("ustack AT_EXECFN string", 0);
        } else {
            check("ustack AT_EXECFN string", 0);
        }
    }

    vmm_space_destroy(as);
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
    test_elf();
    test_ustack();

    if (tests_failed == 0)
        pr_notice("selftest: %d/%d checks passed", tests_run - tests_failed, tests_run);
    else
        pr_err("selftest: %d of %d checks FAILED", tests_failed, tests_run);
}
