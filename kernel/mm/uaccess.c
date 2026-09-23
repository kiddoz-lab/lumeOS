/*
 * LumeOS user memory access.
 *
 * Copying between kernel and user memory is done directly (the user address
 * space is active while a syscall runs), but only after validating the range
 * against the page tables.  A kernel that faults in the middle of a user copy
 * is a kernel bug, so the validation is what makes these helpers safe rather
 * than a fault-recovery mechanism.
 *
 * The _as() forms exist for the case where the address space to touch is *not*
 * the one the MMU is currently using - building the first process's stack at
 * boot, and exec() later.  Validating against a space and then copying through
 * another is not a copy to that space at all: the first version of this file
 * did exactly that, and the kernel took a data abort on the user stack address
 * because the address was valid in the target space and absent from the active
 * one.  So the copy happens with the target space installed, and the previous
 * one is put back afterwards.
 */
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/string.h>
#include <lume/types.h>

/*
 * Install `as` for the duration of one access and hand back the space to
 * restore.  The kernel half of the address space (code, data, stacks, page
 * tables) is mapped in every space, so a switch here cannot pull the ground out
 * from under the running kernel - which is what makes this safe from a boot
 * thread that is in the middle of building a process.
 */
static struct vm_space *space_enter(struct vm_space *as)
{
    struct vm_space *previous = vmm_current_space();

    if (as && as != previous)
        vmm_switch_to(as);
    else
        return NULL;
    return previous;
}

static void space_leave(struct vm_space *previous)
{
    if (previous)
        vmm_switch_to(previous);
}

int copy_to_user_as(struct vm_space *as, void *user_dst, const void *src, u32 len)
{
    struct vm_space *previous;

    if (len == 0)
        return 0;
    if (vmm_check_range(as, (u32)user_dst, len, 1, 1) < 0) {
        pr_debug("uaccess: copy_to_user range %p+%u not writable", user_dst, len);
        return -1;
    }
    previous = space_enter(as);
    memcpy(user_dst, src, len);
    space_leave(previous);
    return 0;
}

int copy_from_user_as(struct vm_space *as, void *dst, const void *user_src, u32 len)
{
    struct vm_space *previous;

    if (len == 0)
        return 0;
    if (vmm_check_range(as, (u32)user_src, len, 0, 1) < 0) {
        pr_debug("uaccess: copy_from_user range %p+%u not readable", user_src, len);
        return -1;
    }
    previous = space_enter(as);
    memcpy(dst, user_src, len);
    space_leave(previous);
    return 0;
}

int clear_user_as(struct vm_space *as, void *user_dst, u32 len)
{
    struct vm_space *previous;

    if (len == 0)
        return 0;
    if (vmm_check_range(as, (u32)user_dst, len, 1, 1) < 0)
        return -1;
    previous = space_enter(as);
    memset(user_dst, 0, len);
    space_leave(previous);
    return 0;
}

int copy_to_user(void *user_dst, const void *src, u32 len)
{
    return copy_to_user_as(NULL, user_dst, src, len);
}

int copy_from_user(void *dst, const void *user_src, u32 len)
{
    return copy_from_user_as(NULL, dst, user_src, len);
}

int clear_user(void *user_dst, u32 len)
{
    return clear_user_as(NULL, user_dst, len);
}

int strncpy_from_user(char *dst, const char *user_src, u32 max)
{
    u32 i;

    for (i = 0; i < max; i++) {
        /* Validate one byte at a time: the string may end at a page boundary
         * and the caller does not know its length up front. */
        if (vmm_check_range(NULL, (u32)(user_src + i), 1, 0, 1) < 0)
            return -1;
        dst[i] = user_src[i];
        if (dst[i] == '\0')
            return (int)(i + 1);
    }
    dst[max - 1] = '\0';
    return (int)max;
}
