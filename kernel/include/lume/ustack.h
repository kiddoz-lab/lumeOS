/*
 * The initial user stack: argv, envp and the auxiliary vector.
 *
 * This is the Linux process-entry contract, and it is exact, because programs
 * that are not ours depend on it.  Lowest address first, with sp pointing at
 * the first word:
 *
 *     sp -> [ argc ]
 *           [ argv[0] ... argv[argc-1] ]
 *           [ 0 ]
 *           [ envp[0] ... envp[envc-1] ]
 *           [ 0 ]
 *           [ auxv: type, value, type, value, ... ]
 *           [ AT_NULL, 0 ]
 *           (padding)
 *           the strings themselves: argv and envp contents, AT_EXECFN,
 *           AT_PLATFORM, and the 16 bytes AT_RANDOM points at
 *
 * Two details are easy to get wrong and are therefore written down here:
 *
 *   - sp is rounded to 16 bytes, not 8.  The ARM ABI (AAPCS) only requires
 *     8-byte alignment at a public interface, but the Linux kernel rounds the
 *     initial stack to 16 and some C runtimes rely on that, so rounding to 16
 *     is what compatibility costs: at most 15 bytes of stack.
 *
 *   - the strings live at *higher* addresses than the tables that point at
 *     them, because the stack grows down and they are pushed first.  A program
 *     walks from sp up through argc/argv/envp to find the auxv, so the order of
 *     the tables is the ABI and the order of the strings is not.
 *
 * The kernel builds this for `init` at boot, and the same code will build it
 * for exec() later; nothing here knows which of those it is doing.
 */
#ifndef LUME_USTACK_H
#define LUME_USTACK_H

#include <lume/mem.h>
#include <lume/types.h>

/* Bounds on what one program may start with.  They exist so the layout can be
 * computed in fixed-size arrays before anything is mapped or written. */
#define LUME_USER_MAX_ARGV 8
#define LUME_USER_MAX_ENVP 8

struct user_startup {
    const char *path;              /* AT_EXECFN: the name it was started with */
    const char **argv;             /* NULL-terminated */
    const char **envp;             /* NULL-terminated; may be NULL */
    u32 entry;                     /* AT_ENTRY */
    u32 phdr;                      /* AT_PHDR: virtual address of the headers */
    u16 phnum;                     /* AT_PHNUM */
    u16 phent;                     /* AT_PHENT */
    u32 uid, euid, gid, egid;      /* AT_UID/AT_EUID/AT_GID/AT_EGID */
};

/*
 * Map the stack for `as`, write argv/envp/auxv into it and return the value sp
 * must have at process entry.  Returns 0 on failure, having mapped nothing that
 * the caller is expected to keep: the address space is left for
 * vmm_space_destroy() to reclaim, which is what the init path does when the
 * stack cannot be built.
 */
u32 user_stack_build(struct vm_space *as, const struct user_startup *start);

/* What the last layout wanted, for logging and the self test. */
struct user_stack_info {
    u32 sp;
    u32 pages;
    u32 auxv_entries;   /* pairs, not counting AT_NULL */
    u32 bytes;
};

u32 user_stack_build_ex(struct vm_space *as, const struct user_startup *start,
                        struct user_stack_info *info_out);

/** How many auxv pairs the kernel writes for a given startup request. */
u32 user_auxv_count(void);

/** Where the auxv ends up, without building anything (used by the self test to
 *  check the layout arithmetic against a built stack). */
int user_stack_layout(const struct user_startup *start,
                      struct user_stack_info *info_out, u32 *sp_out);

#endif /* LUME_USTACK_H */
