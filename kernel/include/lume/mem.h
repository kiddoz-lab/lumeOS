/*
 * LumeOS memory management interfaces.
 *
 * Three layers, from the bottom up:
 *
 *   pmm     - physical page allocator (bitmap, one bit per 4 KiB page)
 *   vmm     - per-address-space page tables (ARMv6 short descriptors)
 *   kmalloc - the kernel's own dynamic heap, carved out of pmm pages
 *
 * The kernel image itself runs from the linear mapping at 0xC0000000 +
 * physical, so any physical address below the end of RAM can be reached with
 * PHYS_TO_VIRT().  That is how page tables and DMA buffers are touched.
 */
#ifndef LUME_MEM_H
#define LUME_MEM_H

#include <lume/compiler.h>
#include <lume/types.h>

/* ------------------------------------------------------------------ */
/* Physical memory manager                                             */
/* ------------------------------------------------------------------ */

void pmm_init(u32 ram_base, u32 ram_end);
void pmm_reserve(u32 base, u32 size);

/** Allocate one 4 KiB page.  Returns its physical address, or 0 on failure. */
u32 pmm_alloc_page(void);

/** Allocate `count` contiguous pages aligned to `align` bytes. */
u32 pmm_alloc_pages(u32 count, u32 align);

void pmm_free_page(u32 pa);
void pmm_free_pages(u32 pa, u32 count);

u32 pmm_total_bytes(void);
u32 pmm_free_bytes(void);
u32 pmm_used_bytes(void);
u32 pmm_page_count(void);

/* ------------------------------------------------------------------ */
/* Virtual memory manager                                              */
/* ------------------------------------------------------------------ */

/* Protection flags (LUME_PROT_* from pte.h) are used for user mappings. */
#define VM_FLAG_USER   (1u << 0)  /* accessible from user mode */
#define VM_FLAG_WRITE  (1u << 1)
#define VM_FLAG_EXEC   (1u << 2)

struct vm_space {
    u32 *l1;         /* kernel virtual address of the level 1 table */
    u32 l1_pa;       /* physical address of the level 1 table */
    u32 user_pages;  /* accounting for /proc-style reporting */
};

/** Build the kernel (swapper) address space and the boot mappings. */
void vmm_init(u32 ram_base, u32 ram_end);

/** The kernel address space. */
struct vm_space *vmm_kernel_space(void);
struct vm_space *vmm_current_space(void);

/** Extend the linear kernel mapping so that all of RAM is reachable. */
void vmm_map_kernel_ram(u32 ram_base, u32 ram_end);

struct vm_space *vmm_space_create(void);
void vmm_space_destroy(struct vm_space *as);

int vmm_map_page(struct vm_space *as, u32 va, u32 pa, u32 flags);
int vmm_map_range(struct vm_space *as, u32 va, u32 pa, u32 size, u32 flags);
int vmm_unmap_page(struct vm_space *as, u32 va);
int vmm_unmap_range(struct vm_space *as, u32 va, u32 size);

/** Map a physical range into the uncached window and return its virtual
 *  address.  Used for the framebuffer and other shared-with-VideoCore
 *  buffers. */
void *vmm_map_uncached(u32 pa, u32 size, u32 *out_va);

/** Switch the active address space (flush the TLB when it changes). */
void vmm_switch_to(struct vm_space *as);

/** Software page table walk; returns 0 when the address is not mapped. */
/*
 * Translate `va` in `as` (or in the current space when `as` is NULL).
 * Returns 0 and stores the physical address in *pa_out on success, -1 when the
 * address is not mapped.
 *
 * The physical address is an out-parameter on purpose: physical address 0 is a
 * perfectly valid mapping (RAM starts there, and the kernel's own alias maps
 * virtual 0xC0000000 to it), so returning 0 as the error indicator would make
 * the bottom of the address space indistinguishable from "unmapped".  The
 * in-kernel self test caught exactly that ambiguity.
 */
int vmm_translate(struct vm_space *as, u32 va, u32 *pa_out);

/** Is [va, va+len) mapped with the requested access in this address space? */
int vmm_check_range(struct vm_space *as, u32 va, u32 len, int write, int user);

/** Physical address of the level 2 table covering va, creating it if needed. */
u32 vmm_get_l2(struct vm_space *as, u32 va, int create);

/* ------------------------------------------------------------------ */
/* Kernel heap                                                         */
/* ------------------------------------------------------------------ */

void kmalloc_init(void);
void *kmalloc(u32 size);
void *kzalloc(u32 size);
void *krealloc(void *ptr, u32 size);
void kfree(void *ptr);

u32 kmalloc_used_bytes(void);
u32 kmalloc_total_bytes(void);

/* ------------------------------------------------------------------ */
/* User memory access helpers                                          */
/* ------------------------------------------------------------------ */

/** Copy to/from a user address in the current address space.  Returns 0 on
 *  success and -1 when the range is not mapped (no fault is taken). */
int copy_to_user(void *user_dst, const void *src, u32 len);
int copy_from_user(void *dst, const void *user_src, u32 len);
int clear_user(void *user_dst, u32 len);
int strncpy_from_user(char *dst, const char *user_src, u32 max);

#endif /* LUME_MEM_H */
