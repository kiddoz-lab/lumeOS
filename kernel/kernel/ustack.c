/*
 * Build the initial user stack: argc, argv, envp and the auxiliary vector.
 *
 * The layout this produces is the Linux process-entry ABI, described in
 * kernel/include/lume/ustack.h.  The work is split in two on purpose:
 *
 *   1. user_stack_layout() decides every address without writing anything.
 *      It has to, because the number of pages to map depends on the size of the
 *      layout, and the size depends on the strings - mapping first would mean
 *      guessing.
 *   2. the build writes the strings and then the tables through
 *      copy_to_user_as(), so every access is validated against the target
 *      address space rather than against "whatever space happens to be active".
 *
 * The address arithmetic is the part worth reading carefully: the strings are
 * pushed from the top of the stack downwards (the stack grows down, and the
 * tables that point at the strings must end up *below* them), and sp is then
 * rounded to 16 bytes to satisfy the ABI with room to spare.
 */
#include <lume/auxv.h>
#include <lume/config.h>
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/random.h>
#include <lume/string.h>
#include <lume/ustack.h>

#define STACK_ALIGN 16          /* see ustack.h: the ABI needs 8, Linux uses 16 */
#define STRING_ALIGN 8          /* pointers to chars must be at least 4; 8 is free */

struct stack_layout {
    u32 sp;                 /* value sp must hold at entry: the argc word */
    u32 base;               /* lowest byte of the mapped stack */
    u32 pages;
    u32 argc, envc;
    u32 argv_va[LUME_USER_MAX_ARGV];
    u32 envp_va[LUME_USER_MAX_ENVP];
    u32 path_va;            /* AT_EXECFN's string */
    u32 platform_va;        /* AT_PLATFORM's string */
    u32 random_va;          /* the 16 bytes AT_RANDOM points at */
};

/* ------------------------------------------------------------------ */
/* The auxiliary vector                                                */
/* ------------------------------------------------------------------ */

/*
 * Number of (type, value) pairs written before AT_NULL.  It is a constant
 * because the table size has to be known before the table is written - and it
 * is *checked* against the emitter by the self test, so the two cannot drift
 * apart silently.
 */
#define LUME_AUXV_PAIRS 17

u32 user_auxv_count(void)
{
    return LUME_AUXV_PAIRS;
}

/** Write one auxv pair (or the terminating AT_NULL/0, which is a pair too). */
static int put_auxv(struct vm_space *as, u32 *w, u32 type, u32 value)
{
    if (copy_to_user_as(as, (void *)*w, &type, 4) < 0)
        return -1;
    *w += 4;
    if (copy_to_user_as(as, (void *)*w, &value, 4) < 0)
        return -1;
    *w += 4;
    return 0;
}

/*
 * What a program is told about the machine it is running on.  Everything here
 * is either a fact this kernel can back up or a deliberate omission; the
 * reasoning for the HWCAP bits and for AT_RANDOM lives in the headers and in
 * docs/userspace.md, not in this function.
 */
static int auxv_emit(struct vm_space *as, u32 *w, const struct user_startup *s,
                     const struct stack_layout *l)
{
    u32 clk = LUME_AT_CLKTCK_VALUE;

    if (put_auxv(as, w, LUME_AT_HWCAP, LUME_HWCAP_ARMv6KZ) < 0)
        return -1;
    if (put_auxv(as, w, LUME_AT_PAGESZ, PAGE_SIZE) < 0)
        return -1;
    if (put_auxv(as, w, LUME_AT_CLKTCK, clk) < 0)
        return -1;
    if (put_auxv(as, w, LUME_AT_PHDR, s->phdr) < 0)
        return -1;
    if (put_auxv(as, w, LUME_AT_PHENT, s->phent) < 0)
        return -1;
    if (put_auxv(as, w, LUME_AT_PHNUM, s->phnum) < 0)
        return -1;
    /* AT_BASE is the interpreter's load address: 0 for a static program, which
     * is the only kind this kernel can start until it grows a dynamic linker. */
    if (put_auxv(as, w, LUME_AT_BASE, 0) < 0)
        return -1;
    if (put_auxv(as, w, LUME_AT_FLAGS, 0) < 0)
        return -1;
    if (put_auxv(as, w, LUME_AT_ENTRY, s->entry) < 0)
        return -1;
    if (put_auxv(as, w, LUME_AT_UID, s->uid) < 0)
        return -1;
    if (put_auxv(as, w, LUME_AT_EUID, s->euid) < 0)
        return -1;
    if (put_auxv(as, w, LUME_AT_GID, s->gid) < 0)
        return -1;
    if (put_auxv(as, w, LUME_AT_EGID, s->egid) < 0)
        return -1;
    /* AT_SECURE: no setuid binaries exist yet (no filesystem, no credentials
     * beyond uid 0), so there is nothing to be careful about - and a program
     * that sees 0 knows not to trust its environment. */
    if (put_auxv(as, w, LUME_AT_SECURE, 0) < 0)
        return -1;
    if (put_auxv(as, w, LUME_AT_RANDOM, l->random_va) < 0)
        return -1;
    if (put_auxv(as, w, LUME_AT_EXECFN, l->path_va) < 0)
        return -1;
    if (put_auxv(as, w, LUME_AT_PLATFORM, l->platform_va) < 0)
        return -1;
    return put_auxv(as, w, LUME_AT_NULL, 0);
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

/** Count a NULL-terminated vector, refusing one longer than the fixed bound. */
static int count_vector(const char *const *v, u32 max, u32 *out)
{
    u32 n = 0;

    if (!v) {
        *out = 0;
        return 0;
    }
    while (v[n]) {
        if (n >= max)
            return -1;
        n++;
    }
    *out = n;
    return 0;
}

/** Push one string from the top of the stack downwards; returns its address. */
static u32 push_string(u32 *pos, const char *text)
{
    u32 len = (u32)strlen(text) + 1;

    *pos -= len;
    *pos &= ~(u32)(STRING_ALIGN - 1);
    return *pos;
}

static int layout_build(const struct user_startup *s, struct stack_layout *l)
{
    u32 pos = LUME_USER_STACK_TOP;
    u32 words, table_bytes, needed;
    u32 i;

    memset(l, 0, sizeof(*l));

    if (!s->argv || !s->path)
        return -1;
    if (count_vector(s->argv, LUME_USER_MAX_ARGV, &l->argc) < 0)
        return -1;
    if (count_vector(s->envp, LUME_USER_MAX_ENVP, &l->envc) < 0)
        return -1;
    if (!IS_ALIGNED(s->phdr, 4))
        return -1;

    /* The strings, highest address first: argv, then envp, then the two names
     * and the random bytes.  Their relative order is not part of the ABI - only
     * the tables' order is - but they must all be above the tables. */
    for (i = l->argc; i > 0; i--)
        l->argv_va[i - 1] = push_string(&pos, s->argv[i - 1]);
    for (i = l->envc; i > 0; i--)
        l->envp_va[i - 1] = push_string(&pos, s->envp[i - 1]);
    l->path_va = push_string(&pos, s->path);
    l->platform_va = push_string(&pos, LUME_ELF_PLATFORM);

    /* The 16 bytes for AT_RANDOM: a pointer, so keep it aligned. */
    pos -= 16;
    pos &= ~(u32)(STRING_ALIGN - 1);
    l->random_va = pos;

    /* The tables: argc, argv[], NULL, envp[], NULL, auxv..., AT_NULL. */
    words = 1 + (l->argc + 1) + (l->envc + 1) + 2 * (LUME_AUXV_PAIRS + 1);
    table_bytes = words * sizeof(u32);
    if (pos < LUME_USER_STACK_TOP - LUME_USER_STACK_MAX)
        return -1;
    l->sp = (pos - table_bytes) & ~(u32)(STACK_ALIGN - 1);

    /* sp is the lowest address the program is allowed to touch; the pages must
     * cover everything from the page containing sp up to the top. */
    if (l->sp < LUME_USER_STACK_TOP - LUME_USER_STACK_MAX)
        return -1;
    needed = LUME_USER_STACK_TOP - PAGE_ALIGN_DOWN(l->sp);
    l->pages = needed / PAGE_SIZE;
    if (l->pages < LUME_USER_STACK_MIN_PAGES)
        l->pages = LUME_USER_STACK_MIN_PAGES;
    if (l->pages > LUME_USER_STACK_MAX / PAGE_SIZE)
        return -1;
    l->base = LUME_USER_STACK_TOP - l->pages * PAGE_SIZE;
    if (l->sp < l->base)
        return -1;      /* cannot happen, but a stack outside its own pages is
                         * not a failure mode worth discovering later */
    return 0;
}

/* ------------------------------------------------------------------ */
/* Build                                                               */
/* ------------------------------------------------------------------ */

/*
 * Map the stack pages, zeroed.
 *
 * The zeroing goes through clear_user_as() rather than through the kernel's
 * linear mapping of the physical page.  Both work, but this one is the same
 * validated path every other write in this file takes, it does not assume that
 * a page allocated from pmm is reachable at PHYS_TO_VIRT (an assumption worth
 * keeping out of code that also runs for a *different* address space than the
 * current one), and it is what makes this file testable on the host - where
 * there is no linear mapping at all.
 */
static int map_pages(struct vm_space *as, u32 base, u32 count)
{
    for (u32 i = 0; i < count; i++) {
        u32 va = base + i * PAGE_SIZE;
        u32 pa = pmm_alloc_page();

        if (!pa)
            return -1;
        if (vmm_map_page(as, va, pa, VM_FLAG_USER | VM_FLAG_WRITE) < 0) {
            pmm_free_page(pa);
            return -1;
        }
        if (clear_user_as(as, (void *)va, PAGE_SIZE) < 0) {
            pmm_free_page(pa);
            return -1;
        }
    }
    return 0;
}

static int put_string(struct vm_space *as, u32 va, const char *text)
{
    return copy_to_user_as(as, (void *)va, text, (u32)strlen(text) + 1);
}

static int put_word(struct vm_space *as, u32 *w, u32 value)
{
    if (copy_to_user_as(as, (void *)*w, &value, 4) < 0)
        return -1;
    *w += 4;
    return 0;
}

u32 user_stack_build_ex(struct vm_space *as, const struct user_startup *start,
                        struct user_stack_info *info_out)
{
    struct stack_layout l;
    u8 random_bytes[16];
    u32 w;
    u32 i;

    if (!as || !start || layout_build(start, &l) < 0)
        return 0;

    if (map_pages(as, l.base, l.pages) < 0)
        return 0;

    /* Strings first: the tables point at them, so they must exist before the
     * program can follow a pointer.  (It also means a failure here leaves a
     * stack whose tables were never written - unreadable, not misleading.) */
    for (i = 0; i < l.argc; i++)
        if (put_string(as, l.argv_va[i], start->argv[i]) < 0)
            return 0;
    for (i = 0; i < l.envc; i++)
        if (put_string(as, l.envp_va[i], start->envp[i]) < 0)
            return 0;
    if (put_string(as, l.path_va, start->path) < 0)
        return 0;
    if (put_string(as, l.platform_va, LUME_ELF_PLATFORM) < 0)
        return 0;

    lume_random_bytes(random_bytes, sizeof(random_bytes));
    if (copy_to_user_as(as, (void *)l.random_va, random_bytes,
                        sizeof(random_bytes)) < 0)
        return 0;

    w = l.sp;
    if (put_word(as, &w, l.argc) < 0)
        return 0;
    for (i = 0; i < l.argc; i++)
        if (put_word(as, &w, l.argv_va[i]) < 0)
            return 0;
    if (put_word(as, &w, 0) < 0)
        return 0;
    for (i = 0; i < l.envc; i++)
        if (put_word(as, &w, l.envp_va[i]) < 0)
            return 0;
    if (put_word(as, &w, 0) < 0)
        return 0;
    if (auxv_emit(as, &w, start, &l) < 0)
        return 0;

    if (info_out) {
        info_out->sp = l.sp;
        info_out->pages = l.pages;
        info_out->auxv_entries = LUME_AUXV_PAIRS;
        info_out->bytes = LUME_USER_STACK_TOP - l.sp;
    }
    return l.sp;
}

u32 user_stack_build(struct vm_space *as, const struct user_startup *start)
{
    return user_stack_build_ex(as, start, NULL);
}

int user_stack_layout(const struct user_startup *start,
                      struct user_stack_info *info_out, u32 *sp_out)
{
    struct stack_layout l;

    if (!start || layout_build(start, &l) < 0)
        return -1;
    if (info_out) {
        info_out->sp = l.sp;
        info_out->pages = l.pages;
        info_out->auxv_entries = LUME_AUXV_PAIRS;
        info_out->bytes = LUME_USER_STACK_TOP - l.sp;
    }
    if (sp_out)
        *sp_out = l.sp;
    return 0;
}
