/*
 * LumeOS ARMv6 MMU support.
 *
 * The Raspberry Pi Zero W's BCM2835 implements the ARMv6 short-descriptor
 * translation scheme: a 16 KiB level 1 table with 4096 entries, each covering
 * 1 MiB either as a section or as a pointer to a 1 KiB level 2 table with 256
 * small page (4 KiB) entries.
 *
 * LumeOS uses:
 *   - 1 MiB sections for everything that is static and large (the kernel RAM
 *     window, the peripheral window, the vector alias),
 *   - 4 KiB pages for everything that belongs to a process or needs
 *     per-page attributes (user memory, uncached framebuffer/DMA buffers).
 *
 * The level 1 table for the running kernel lives at physical 0x4000 (set up
 * by boot.S).  Per-process level 1 tables are page-aligned allocations from
 * the physical allocator; the kernel half (entries 0xC00-0xFFF) is copied from
 * the swapper table when a space is created, which is how the kernel stays
 * mapped while a user process runs.
 *
 * Reference: ARM1176JZF-S Technical Reference Manual, chapter 6 "Memory
 * Management Unit", in particular the descriptor formats in tables 6-2 and
 * 6-3 and the translation table base register in 3.2.9.
 */
#include <lume/asm.h>
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/panic.h>
#include <lume/pte.h>
#include <lume/string.h>
#include <lume/types.h>

/* The boot page table, defined by boot.S. */
#define BOOT_L1_PA 0x00004000u

/* Kernel half of the level 1 table: entries 0xC00..0xFFF (0xC0000000 up). */
#define L1_KERNEL_FIRST 0xC00u
#define L1_KERNEL_LAST  0xFFFu

/* Uncached window used for buffers shared with the VideoCore (framebuffer,
 * mailbox-style DMA).  32 MiB starting at 0xE0000000 (see types.h). */
#define UNCACHED_VBASE 0xE0000000u
#define UNCACHED_SIZE  0x02000000u
#define L1_INDEX(va) ((va) >> 20)

static struct vm_space kernel_space;
static struct vm_space *current_space;
static u32 uncached_next;      /* bump pointer inside the uncached window */

static inline u32 *l1_of(struct vm_space *as)
{
    return as->l1;
}

static inline u32 l1_entry(u32 va)
{
    return va >> 20;
}

static inline u32 l2_entry(u32 va)
{
    return (va >> 12) & 0xFFu;
}

/* ------------------------------------------------------------------ */
/* Boot time setup                                                     */
/* ------------------------------------------------------------------ */

void vmm_map_kernel_ram(u32 ram_base, u32 ram_end)
{
    u32 *l1 = kernel_space.l1;
    u32 first_pa = ram_base & 0xFFF00000u;
    u32 last_pa = (ram_end - 1) & 0xFFF00000u;

    /* Virtual 0xC0000000 maps physical 0x00000000, so the level 1 index for a
     * physical address is L1_KERNEL_FIRST + (pa >> 20). */
    for (u32 pa = first_pa; pa <= last_pa; pa += SECTION_SIZE) {
        u32 idx = L1_KERNEL_FIRST + (pa >> SECTION_SHIFT);

        if (idx > L1_KERNEL_LAST)
            break;
        l1[idx] = pa | L1_SECT_KERNEL_ROM;
    }
    arm_tlb_invalidate_all();
}

void vmm_init(u32 ram_base, u32 ram_end)
{
    kernel_space.l1 = (u32 *)PHYS_TO_VIRT(BOOT_L1_PA);
    kernel_space.l1_pa = BOOT_L1_PA;
    kernel_space.user_pages = 0;
    current_space = &kernel_space;
    uncached_next = UNCACHED_VBASE;

    vmm_map_kernel_ram(ram_base, ram_end);

    /* Create the uncached window's level 2 table and point the level 1 entry
     * at it.  Individual physical ranges are then mapped into it on demand. */
    u32 l2_pa = pmm_alloc_page();
    if (!l2_pa)
        panic("vmm: cannot allocate the uncached window page table");
    u32 *l2 = (u32 *)PHYS_TO_VIRT(l2_pa);
    memset(l2, 0, PAGE_SIZE);
    l1_of(&kernel_space)[L1_INDEX(UNCACHED_VBASE)] =
        l2_pa | L1_TYPE_PAGETABLE | L1_SECT_DOMAIN(0);
    arm_tlb_invalidate_all();

    pr_info("vmm: kernel space at %p, uncached window %p..%p",
            l1_of(&kernel_space), (void *)UNCACHED_VBASE,
            (void *)(UNCACHED_VBASE + UNCACHED_SIZE));
}

struct vm_space *vmm_kernel_space(void)
{
    return &kernel_space;
}

struct vm_space *vmm_current_space(void)
{
    return current_space;
}

/* ------------------------------------------------------------------ */
/* Address spaces                                                      */
/* ------------------------------------------------------------------ */

struct vm_space *vmm_space_create(void)
{
    struct vm_space *as = kmalloc(sizeof(struct vm_space));
    u32 l1_pa;
    u32 *l1;
    u32 flags;

    if (!as)
        return NULL;

    /* The level 1 table is 16 KiB and must be aligned to its size: the low
     * 14 bits of TTBR0 are ignored by the hardware. */
    l1_pa = pmm_alloc_pages(L1_TABLE_SIZE / PAGE_SIZE, L1_TABLE_SIZE);
    if (!l1_pa) {
        kfree(as);
        return NULL;
    }

    l1 = (u32 *)PHYS_TO_VIRT(l1_pa);
    memset(l1, 0, L1_TABLE_SIZE);

    /* Copy the kernel half so that the kernel stays mapped while this space
     * is active.  User space (entries 0..0xBFF) starts out unmapped. */
    flags = arm_irq_save();
    for (u32 i = L1_KERNEL_FIRST; i <= L1_KERNEL_LAST; i++)
        l1[i] = l1_of(&kernel_space)[i];
    arm_irq_restore(flags);

    as->l1 = l1;
    as->l1_pa = l1_pa;
    as->user_pages = 0;
    return as;
}

void vmm_space_destroy(struct vm_space *as)
{
    u32 *l1;

    if (!as || as == &kernel_space)
        return;

    l1 = l1_of(as);
    /* Free the user half's level 2 tables and the mapped pages themselves. */
    for (u32 i = 0; i < L1_KERNEL_FIRST; i++) {
        u32 entry = l1[i];

        if ((entry & L1_TYPE_MASK) == L1_TYPE_PAGETABLE) {
            u32 l2_pa = entry & 0xFFFFFC00u;
            u32 *l2 = (u32 *)PHYS_TO_VIRT(l2_pa);

            for (u32 j = 0; j < L2_TABLE_ENTRIES; j++) {
                u32 pte = l2[j];
                if ((pte & L2_TYPE_MASK) == L2_TYPE_SMALL_PAGE ||
                    (pte & L2_TYPE_MASK) == L2_TYPE_SMALL_PAGE_XN)
                    pmm_free_page(pte & 0xFFFFF000u);
            }
            pmm_free_page(l2_pa);
        }
    }
    pmm_free_pages(as->l1_pa, L1_TABLE_SIZE / PAGE_SIZE);
    kfree(as);
}

/* ------------------------------------------------------------------ */
/* Mapping                                                             */
/* ------------------------------------------------------------------ */

u32 vmm_get_l2(struct vm_space *as, u32 va, int create)
{
    u32 *l1 = l1_of(as);
    u32 idx = l1_entry(va);
    u32 entry = l1[idx];

    if ((entry & L1_TYPE_MASK) == L1_TYPE_PAGETABLE)
        return entry & 0xFFFFFC00u;

    if (!create)
        return 0;

    u32 l2_pa = pmm_alloc_page();
    if (!l2_pa)
        return 0;

    memset((void *)PHYS_TO_VIRT(l2_pa), 0, PAGE_SIZE);
    l1[idx] = l2_pa | L1_TYPE_PAGETABLE | L1_SECT_DOMAIN(LUME_DOMAIN);
    arm_tlb_invalidate_mva((void *)va);
    return l2_pa;
}

static u32 prot_to_l2_flags(u32 flags)
{
    u32 pte = L2_TYPE_SMALL_PAGE; /* a 4 KiB small page descriptor */

    pte |= L2_SMALL_C_BIT | L2_SMALL_B_BIT; /* normal, write-back, cacheable */
    if (flags & VM_FLAG_WRITE)
        pte |= L2_AP_USER_RW;
    else
        pte |= L2_AP_USER_RO;

    if (!(flags & VM_FLAG_USER))
        pte = (pte & ~(L2_SMALL_AP0 | L2_SMALL_AP1)) | L2_SMALL_AP0; /* supervisor RW */

    if (!(flags & VM_FLAG_EXEC)) {
        /* ARMv6 encodes execute-never in the descriptor type: type 3 is a
         * small page with XN set (TRM table 6-3). */
        pte = (pte & ~L2_TYPE_MASK) | L2_TYPE_SMALL_PAGE_XN;
    }
    return pte;
}

int vmm_map_page(struct vm_space *as, u32 va, u32 pa, u32 flags)
{
    u32 l2_pa;
    u32 *l2;
    u32 flags_saved;

    if (!as)
        as = current_space;
    if (!IS_ALIGNED(va, PAGE_SIZE) || !IS_ALIGNED(pa, PAGE_SIZE))
        return -1;

    flags_saved = arm_irq_save();
    l2_pa = vmm_get_l2(as, va, 1);
    if (!l2_pa) {
        arm_irq_restore(flags_saved);
        return -1;
    }

    l2 = (u32 *)PHYS_TO_VIRT(l2_pa);
    l2[l2_entry(va)] = (pa & 0xFFFFF000u) | prot_to_l2_flags(flags) | L2_SMALL_NG;
    as->user_pages++;

    /* ARMv6 has no "invalidate by ASID": if this space is not the current one
     * the stale entry cannot matter until it runs, and vmm_switch_to() flushes
     * the TLB.  For the current space, invalidate the affected entry now. */
    if (as == current_space)
        arm_tlb_invalidate_mva((void *)va);
    arm_irq_restore(flags_saved);
    return 0;
}

int vmm_map_range(struct vm_space *as, u32 va, u32 pa, u32 size, u32 flags)
{
    u32 off;

    for (off = 0; off < size; off += PAGE_SIZE)
        if (vmm_map_page(as, va + off, pa + off, flags) < 0)
            return -1;
    return 0;
}

int vmm_unmap_page(struct vm_space *as, u32 va)
{
    u32 l2_pa;
    u32 *l2;
    u32 flags_saved;

    if (!as)
        as = current_space;

    flags_saved = arm_irq_save();
    l2_pa = vmm_get_l2(as, va, 0);
    if (!l2_pa) {
        arm_irq_restore(flags_saved);
        return -1;
    }

    l2 = (u32 *)PHYS_TO_VIRT(l2_pa);
    if ((l2[l2_entry(va)] & L2_TYPE_MASK) != L2_TYPE_FAULT) {
        u32 pa = l2[l2_entry(va)] & 0xFFFFF000u;

        l2[l2_entry(va)] = 0;
        if (as->user_pages)
            as->user_pages--;
        if (as == current_space)
            arm_tlb_invalidate_mva((void *)va);
        pmm_free_page(pa);
    }
    arm_irq_restore(flags_saved);
    return 0;
}

int vmm_unmap_range(struct vm_space *as, u32 va, u32 size)
{
    u32 off;

    for (off = 0; off < size; off += PAGE_SIZE)
        vmm_unmap_page(as, va + off);
    return 0;
}

void *vmm_map_uncached(u32 pa, u32 size, u32 *out_va)
{
    u32 va = uncached_next;
    u32 flags_saved = arm_irq_save();
    u32 l2_pa;
    u32 *l2;
    u32 base, end;

    if (va + size > UNCACHED_VBASE + UNCACHED_SIZE) {
        arm_irq_restore(flags_saved);
        pr_err("vmm: uncached window exhausted");
        return NULL;
    }

    l2_pa = vmm_get_l2(&kernel_space, va, 1);
    if (!l2_pa) {
        arm_irq_restore(flags_saved);
        return NULL;
    }
    l2 = (u32 *)PHYS_TO_VIRT(l2_pa);

    base = PAGE_ALIGN_DOWN(pa);
    end = PAGE_ALIGN_UP(pa + size);
    for (u32 off = 0; off < (end - base); off += PAGE_SIZE) {
        /* Device memory: TEX=000, C=0, B=1 (BCM2835-framebuffer style), not
         * cached, not executable. */
        u32 pte = (L2_TYPE_SMALL_PAGE_XN) | L2_SMALL_B_BIT | L2_SMALL_AP0 |
                  L2_SMALL_NG;
        u32 idx = l2_entry(va + off);
        l2[idx] = ((base + off) & 0xFFFFF000u) | pte;
    }

    arm_tlb_invalidate_mva((void *)va);
    arm_irq_restore(flags_saved);

    uncached_next = ALIGN_UP(va + (end - base), PAGE_SIZE);
    if (out_va)
        *out_va = va;
    return (void *)(va + (pa - base));
}

void vmm_switch_to(struct vm_space *as)
{
    if (!as)
        as = &kernel_space;
    if (as == current_space)
        return;

    /* ARMv6 has no ASID-scoped invalidation, so a full TLB flush is the
     * correct (if conservative) thing to do on every address space switch.
     * See docs/architecture.md for the ASID plan. */
    arm_write_ttbr0(as->l1_pa);
    arm_tlb_invalidate_all();
    current_space = as;
}

u32 vmm_translate(struct vm_space *as, u32 va)
{
    u32 *l1;
    u32 entry;
    u32 l2_pa;
    u32 *l2;

    if (!as)
        as = current_space;
    l1 = l1_of(as);
    entry = l1[l1_entry(va)];

    switch (entry & L1_TYPE_MASK) {
    case L1_TYPE_SECTION:
        return (entry & 0xFFF00000u) | (va & 0xFFFFFu);
    case L1_TYPE_PAGETABLE:
        l2_pa = entry & 0xFFFFFC00u;
        l2 = (u32 *)PHYS_TO_VIRT(l2_pa);
        entry = l2[l2_entry(va)];
        if ((entry & L2_TYPE_MASK) == L2_TYPE_SMALL_PAGE ||
            (entry & L2_TYPE_MASK) == L2_TYPE_SMALL_PAGE_XN)
            return (entry & 0xFFFFF000u) | (va & 0xFFFu);
        return 0;
    default:
        return 0;
    }
}

int vmm_check_range(struct vm_space *as, u32 va, u32 len, int write, int user)
{
    u32 addr;

    if (len == 0)
        return 0;
    /* Guard against wrap-around. */
    if (va + len < va)
        return -1;

    for (addr = PAGE_ALIGN_DOWN(va); addr < va + len; addr += PAGE_SIZE) {
        u32 entry;
        u32 l1e;

        if (!as)
            as = current_space;
        l1e = l1_of(as)[l1_entry(addr)];
        entry = l1e;

        if ((entry & L1_TYPE_MASK) == L1_TYPE_SECTION) {
            if (user && !(entry & L1_SECT_AP_USER))
                return -1;
            if (write && !(entry & L1_SECT_AP_WRITE))
                return -1;
            continue;
        }
        if ((entry & L1_TYPE_MASK) != L1_TYPE_PAGETABLE)
            return -1;
        {
            u32 *l2 = (u32 *)PHYS_TO_VIRT(entry & 0xFFFFFC00u);
            entry = l2[l2_entry(addr)];
            if ((entry & L2_TYPE_MASK) != L2_TYPE_SMALL_PAGE &&
                (entry & L2_TYPE_MASK) != L2_TYPE_SMALL_PAGE_XN)
                return -1;
            if (user && !(entry & L2_SMALL_AP1))
                return -1;
            if (write && !(entry & L2_SMALL_AP0))
                return -1;
        }
    }
    return 0;
}
