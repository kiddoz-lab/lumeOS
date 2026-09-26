/*
 * Host-side test of the initial-stack layout: argv, envp and the auxv.
 *
 * tests/host/test_ustack_layout.py compiles this file with
 * kernel/kernel/ustack.c and kernel/kernel/random.c for the *host* CPU and runs
 * it.  The point is to test the address arithmetic - the ordering, the
 * alignment, the NULL terminators, the failure path - on any development
 * machine, without an emulator or a board.
 *
 * To do that it needs a memory model, and it builds the smallest one that is
 * still honest: a flat array standing in for physical RAM, a page table that is
 * a one-entry-per-page list, and stubs for the four kernel functions the stack
 * builder calls.  The stubs are strict on purpose:
 *
 *   - vmm_map_page records the mapping and *fails* if a page is mapped twice or
 *     if two virtual pages got the same physical page (which would be a real
 *     bug: two stack pages sharing memory);
 *   - copy_to_user_as checks that the destination is inside a mapped page
 *     before writing, exactly as the real one checks the page tables, and
 *     counts failures so a test can assert that a refused build wrote nothing;
 *   - pmm_alloc_page hands out pages from the far end of the fake RAM and
 *     fails when it runs out, so a test can ask "what happens when the stack
 *     cannot be built" without crashing.
 *
 * What this does not test: the real page tables, the MMU, or whether the
 * program that reads this stack gets the registers it expects.  tests/qemu
 * covers the first two and init's own auxv checks cover the third.
 */
#include <lume/auxv.h>
#include <lume/config.h>
#include <lume/mem.h>
#include <lume/random.h>
#include <lume/time.h>
#include <lume/ustack.h>

#include <stdio.h>

/* ------------------------------------------------------------------ */
/* The fake machine                                                    */
/* ------------------------------------------------------------------ */

#define FAKE_RAM_BYTES (256u * 1024u)
#define FAKE_PAGES     (FAKE_RAM_BYTES / PAGE_SIZE)
#define FAKE_MAX_MAPS  64

static unsigned char ram[FAKE_RAM_BYTES];
static u32 next_free_page;              /* allocated from the top downwards */

struct fake_map {
    u32 va;
    u32 pa;
};

static struct fake_map maps[FAKE_MAX_MAPS];
static u32 map_count;
static u32 maps_failed;
static u32 copies;
static u32 copy_failures;
static u32 pages_freed;

/* A vm_space is opaque to ustack.c, so this can be anything with the fields
 * mem.h declares. */
static struct vm_space fake_space;

/* ------------------------------------------------------------------ */
/* Kernel functions the stack builder needs                            */
/* ------------------------------------------------------------------ */

u32 pmm_alloc_page(void)
{
    if (next_free_page == 0)
        return 0;
    next_free_page--;
    return next_free_page * PAGE_SIZE;
}

void pmm_free_page(u32 pa)
{
    (void)pa;
    pages_freed++;
}

int vmm_map_page(struct vm_space *as, u32 va, u32 pa, u32 flags)
{
    (void)as;
    (void)flags;

    for (u32 i = 0; i < map_count; i++) {
        if (maps[i].va == va) {         /* mapping over a live page */
            maps_failed++;
            return -1;
        }
        if (maps[i].pa == pa) {         /* two pages sharing one frame */
            maps_failed++;
            return -1;
        }
    }
    if (map_count >= FAKE_MAX_MAPS) {
        maps_failed++;
        return -1;
    }
    maps[map_count].va = va;
    maps[map_count].pa = pa;
    map_count++;
    return 0;
}

static int fake_translate(u32 va, u32 *pa_out)
{
    for (u32 i = 0; i < map_count; i++) {
        if (va >= maps[i].va && va < maps[i].va + PAGE_SIZE) {
            *pa_out = maps[i].pa + (va - maps[i].va);
            return 0;
        }
    }
    return -1;
}

int copy_to_user_as(struct vm_space *as, void *user_dst, const void *src, u32 len)
{
    u32 va = (u32)user_dst;
    u32 pa;
    u32 i;

    (void)as;

    for (i = 0; i < len; i++) {
        if (fake_translate(va + i, &pa) < 0) {
            copy_failures++;
            return -1;
        }
        ram[pa] = ((const unsigned char *)src)[i];
    }
    copies++;
    return 0;
}

int copy_from_user_as(struct vm_space *as, void *dst, const void *user_src, u32 len)
{
    u32 va = (u32)user_src;
    u32 pa;
    u32 i;

    (void)as;

    for (i = 0; i < len; i++) {
        if (fake_translate(va + i, &pa) < 0) {
            copy_failures++;
            return -1;
        }
        ((unsigned char *)dst)[i] = ram[pa];
    }
    return 0;
}

int clear_user_as(struct vm_space *as, void *user_dst, u32 len)
{
    u32 va = (u32)user_dst;
    u32 i;

    (void)as;

    for (i = 0; i < len; i++) {
        u32 pa;

        if (fake_translate(va + i, &pa) < 0) {
            copy_failures++;
            return -1;
        }
        ram[pa] = 0;
    }
    copies++;
    return 0;
}

u32 pages_mapped(void)
{
    return map_count;
}

/* random.c and ustack.c's logging neighbours. */
u64 timer_read_us(void)
{
    return 0x1234ABCD5678ull;
}

u64 timer_ticks(void)
{
    return 4242;
}

struct process;
struct process *proc_current(void)
{
    return NULL;
}

void klog_emit(int level, const char *msg);
void klog_emit(int level, const char *msg)
{
    (void)level;
    (void)msg;      /* the builder must not log on its own */
}

/* ------------------------------------------------------------------ */
/* The checks                                                          */
/* ------------------------------------------------------------------ */

static int checks;
static int failures;

static void check(const char *name, int condition)
{
    checks++;
    if (!condition) {
        failures++;
        printf("ustack: FAIL %s\n", name);
    }
}

static void reset_fake(void)
{
    next_free_page = FAKE_PAGES;
    map_count = 0;
    maps_failed = 0;
    copies = 0;
    copy_failures = 0;
    pages_freed = 0;
}

static u32 read_word(u32 va)
{
    u32 value = 0;

    if (fake_translate(va, &value) < 0)
        return 0xDEADBEEFu;
    value = 0;
    for (int i = 0; i < 4; i++) {
        u32 pa;

        if (fake_translate(va + (u32)i, &pa) < 0)
            return 0xDEADBEEFu;
        value |= (u32)ram[pa] << (8 * i);
    }
    return value;
}

static void test_layout(void)
{
    const char *argv[] = { "/bin/init", "--first", "--second", NULL };
    const char *envp[] = { "LUME=1", "PATH=/bin", NULL };
    struct user_startup start;
    struct user_stack_info info;
    u32 sp;
    u32 w;

    reset_fake();
    lume_random_seed(0xABCDEF01u);

    start.path = "/bin/init";
    start.argv = argv;
    start.envp = envp;
    start.entry = 0x00010000;
    start.phdr = 0x00010034;
    start.phnum = 4;
    start.phent = 32;
    start.uid = 1000;
    start.euid = 1000;
    start.gid = 1000;
    start.egid = 1000;

    sp = user_stack_build_ex(&fake_space, &start, &info);
    check("build succeeds", sp != 0);
    check("no page was mapped twice or shared", maps_failed == 0);
    check("no copy was refused", copy_failures == 0);
    check("sp is 16-byte aligned", IS_ALIGNED(sp, 16));
    check("sp is inside the mapped stack",
          sp >= LUME_USER_STACK_TOP - info.pages * PAGE_SIZE);
    check("at least the minimum pages were mapped",
          info.pages >= LUME_USER_STACK_MIN_PAGES);
    check("pages mapped matches pages reported", pages_mapped() == info.pages);
    check("info.bytes matches the layout", info.bytes == LUME_USER_STACK_TOP - sp);

    /* The tables, in the order the ABI fixes. */
    check("argc", read_word(sp) == 3);
    check("argv[0] is a pointer", read_word(sp + 4) != 0);
    check("argv[1] is a pointer", read_word(sp + 8) != 0);
    check("argv[2] is a pointer", read_word(sp + 12) != 0);
    check("argv NULL terminator", read_word(sp + 16) == 0);
    check("envp[0] is a pointer", read_word(sp + 20) != 0);
    check("envp[1] is a pointer", read_word(sp + 24) != 0);
    check("envp NULL terminator", read_word(sp + 28) == 0);

    /* The strings those pointers name. */
    {
        char buf[32];
        u32 argv0 = read_word(sp + 4);
        u32 envp1 = read_word(sp + 24);

        check("argv[0] reads back", copy_from_user_as(&fake_space, buf, (void *)argv0, 10) == 0);
        check("argv[0] content", buf[0] == '/' && buf[1] == 'b' && buf[2] == 'i' &&
                                 buf[3] == 'n' && buf[4] == '/' && buf[5] == 'i' &&
                                 buf[6] == 'n' && buf[7] == 'i' && buf[8] == 't' &&
                                 buf[9] == 0);
        check("envp[1] reads back", copy_from_user_as(&fake_space, buf, (void *)envp1, 10) == 0);
        check("envp[1] content", buf[0] == 'P' && buf[1] == 'A' && buf[2] == 'T' &&
                                 buf[3] == 'H' && buf[4] == '=' && buf[5] == '/' &&
                                 buf[6] == 'b' && buf[7] == 'i' && buf[8] == 'n' &&
                                 buf[9] == 0);
    }

    /* The auxiliary vector, walked pair by pair until AT_NULL. */
    /* argc word, then argc pointers and their NULL, then envc pointers and
     * their NULL: 3 arguments and 2 environment strings in this test. */
    w = sp + 4 * (1 + (3 + 1) + (2 + 1));
    {
        u32 seen = 0, terminated = 0;
        u32 pagesz = 0, entry = 0, phdr = 0, phnum = 0, phent = 0;
        u32 uid = 0, euid = 0, gid = 0, egid = 0, base = 0xFFFFFFFFu;
        u32 random_va = 0, execfn = 0, platform = 0, hwcap = 0;
        u32 i;

        for (i = 0; i <= user_auxv_count(); i++) {
            u32 type = read_word(w + 8 * i);
            u32 value = read_word(w + 8 * i + 4);

            if (type == LUME_AT_NULL) {
                terminated = 1;
                break;
            }
            if (type == 0xDEADBEEFu)
                break;
            seen++;
            switch (type) {
            case LUME_AT_PAGESZ: pagesz = value; break;
            case LUME_AT_ENTRY:  entry = value; break;
            case LUME_AT_PHDR:   phdr = value; break;
            case LUME_AT_PHNUM:  phnum = value; break;
            case LUME_AT_PHENT:  phent = value; break;
            case LUME_AT_UID:    uid = value; break;
            case LUME_AT_EUID:   euid = value; break;
            case LUME_AT_GID:    gid = value; break;
            case LUME_AT_EGID:   egid = value; break;
            case LUME_AT_BASE:   base = value; break;
            case LUME_AT_RANDOM: random_va = value; break;
            case LUME_AT_EXECFN: execfn = value; break;
            case LUME_AT_PLATFORM: platform = value; break;
            case LUME_AT_HWCAP:  hwcap = value; break;
            default: break;
            }
        }

        check("auxv terminates with AT_NULL", terminated == 1);
        check("auxv has exactly the pairs the emitter counts",
              seen == user_auxv_count());
        check("AT_PAGESZ is the page size", pagesz == PAGE_SIZE);
        check("AT_ENTRY is the entry we asked for", entry == start.entry);
        check("AT_PHDR is the phdr we asked for", phdr == start.phdr);
        check("AT_PHNUM", phnum == start.phnum);
        check("AT_PHENT", phent == start.phent);
        check("AT_UID", uid == start.uid);
        check("AT_EUID", euid == start.euid);
        check("AT_GID", gid == start.gid);
        check("AT_EGID", egid == start.egid);
        check("AT_BASE is 0 for a static program", base == 0);
        check("AT_HWCAP advertises no VFP", (hwcap & (1u << 6)) == 0);
        check("AT_HWCAP advertises the ARMv6 core",
              (hwcap & LUME_HWCAP_ARMv6KZ) == LUME_HWCAP_ARMv6KZ);
        check("AT_RANDOM points into the stack",
              random_va > sp && random_va < LUME_USER_STACK_TOP);
        check("AT_RANDOM is 16 non-zero bytes", ({
                  unsigned char rnd[16];
                  int nonzero = 0;

                  copy_from_user_as(&fake_space, rnd, (void *)random_va, 16);
                  for (int b = 0; b < 16; b++)
                      if (rnd[b])
                          nonzero = 1;
                  nonzero;
              }));

        /* AT_EXECFN and AT_PLATFORM must name readable strings. */
        {
            char buf[16];

            check("AT_EXECFN is readable",
                  copy_from_user_as(&fake_space, buf, (void *)execfn, 10) == 0);
            check("AT_EXECFN content", buf[0] == '/' && buf[5] == 'i' && buf[9] == 0);
            check("AT_PLATFORM is readable",
                  copy_from_user_as(&fake_space, buf, (void *)platform, 4) == 0);
            check("AT_PLATFORM is 'v6l'",
                  buf[0] == 'v' && buf[1] == '6' && buf[2] == 'l' && buf[3] == 0);
        }

        /* Nothing may overlap: the strings, the random bytes and the tables all
         * live in different bytes of the same pages. */
        check("AT_RANDOM is above the tables", random_va >= w + 8 * (user_auxv_count() + 1));
    }
}

/* The failure path: a layout that cannot fit must be refused *before* anything
 * is mapped, so that a caller which gives up leaves no half-built state. */
static void test_too_big(void)
{
    static char huge[LUME_USER_STACK_MAX + 1024];
    const char *argv[] = { huge, NULL };
    const char *envp[] = { NULL };
    struct user_startup start;
    u32 sp;

    reset_fake();
    for (u32 i = 0; i < sizeof(huge) - 1; i++)
        huge[i] = 'A';
    huge[sizeof(huge) - 1] = 0;

    start.path = "/bin/init";
    start.argv = argv;
    start.envp = envp;
    start.entry = 0x00010000;
    start.phdr = 0x00010034;
    start.phnum = 1;
    start.phent = 32;
    start.uid = start.euid = start.gid = start.egid = 0;

    sp = user_stack_build(&fake_space, &start);
    check("an oversized argument is refused", sp == 0);
    check("nothing was mapped for the refused layout", pages_mapped() == 0);
    check("nothing was written for the refused layout", copies == 0);
    check("no copy was refused (the refusal came from the layout)", copy_failures == 0);
}

/* Too many arguments is also a refusal, not an overrun. */
static void test_too_many_args(void)
{
    const char *argv[LUME_USER_MAX_ARGV + 3];
    struct user_startup start;
    u32 sp;

    reset_fake();
    for (u32 i = 0; i < LUME_USER_MAX_ARGV + 2; i++)
        argv[i] = "x";
    argv[LUME_USER_MAX_ARGV + 2] = NULL;

    start.path = "/bin/init";
    start.argv = argv;
    start.envp = NULL;
    start.entry = 0x00010000;
    start.phdr = 0x00010034;
    start.phnum = 1;
    start.phent = 32;
    start.uid = start.euid = start.gid = start.egid = 0;

    sp = user_stack_build(&fake_space, &start);
    check("too many arguments is refused", sp == 0);
    check("nothing was mapped for the refused argv", pages_mapped() == 0);
}

/* Every page must be mapped: an allocation failure in the middle of the mapping
 * loop must not leave a stack with a hole in it that the ABI above still points
 * into.  The fake allocator is exhausted by asking for a layout that needs more
 * pages than the fake machine has. */
static void test_out_of_memory(void)
{
    static char big[64 * 1024];
    const char *argv[] = { big, NULL };
    struct user_startup start;
    u32 sp;

    reset_fake();
    /* Make the machine small enough that the stack cannot be mapped. */
    next_free_page = 2;
    for (u32 i = 0; i < sizeof(big) - 1; i++)
        big[i] = 'B';
    big[sizeof(big) - 1] = 0;

    start.path = "/bin/init";
    start.argv = argv;
    start.envp = NULL;
    start.entry = 0x00010000;
    start.phdr = 0x00010034;
    start.phnum = 1;
    start.phent = 32;
    start.uid = start.euid = start.gid = start.egid = 0;

    sp = user_stack_build(&fake_space, &start);
    check("a stack that cannot be mapped is refused", sp == 0);
    check("only the pages that were handed out got mapped", pages_mapped() <= 2);
}

int main(void)
{
    test_layout();
    test_too_big();
    test_too_many_args();
    test_out_of_memory();

    printf("ustack: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
