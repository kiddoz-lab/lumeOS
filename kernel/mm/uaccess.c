/*
 * LumeOS user memory access.
 *
 * Copying between kernel and user memory is done directly (the user address
 * space is active while a syscall runs), but only after validating the range
 * against the page tables.  A kernel that faults in the middle of a user copy
 * is a kernel bug, so the validation is what makes these helpers safe rather
 * than a fault-recovery mechanism.
 */
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/string.h>
#include <lume/types.h>

int copy_to_user_as(struct vm_space *as, void *user_dst, const void *src, u32 len)
{
    if (len == 0)
        return 0;
    if (vmm_check_range(as, (u32)user_dst, len, 1, 1) < 0) {
        pr_debug("uaccess: copy_to_user range %p+%u not writable", user_dst, len);
        return -1;
    }
    memcpy(user_dst, src, len);
    return 0;
}

int copy_from_user_as(struct vm_space *as, void *dst, const void *user_src, u32 len)
{
    if (len == 0)
        return 0;
    if (vmm_check_range(as, (u32)user_src, len, 0, 1) < 0) {
        pr_debug("uaccess: copy_from_user range %p+%u not readable", user_src, len);
        return -1;
    }
    memcpy(dst, user_src, len);
    return 0;
}

int clear_user_as(struct vm_space *as, void *user_dst, u32 len)
{
    if (len == 0)
        return 0;
    if (vmm_check_range(as, (u32)user_dst, len, 1, 1) < 0)
        return -1;
    memset(user_dst, 0, len);
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
