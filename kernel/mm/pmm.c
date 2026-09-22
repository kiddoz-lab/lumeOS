/*
 * LumeOS physical page allocator.
 *
 * A flat bitmap with one bit per 4 KiB page ("1" = allocated/reserved).  For
 * the 512 MiB of a Raspberry Pi Zero W that is 16 KiB of bitmap, allocated
 * statically so the allocator works before the heap exists.
 *
 * Allocation is "next fit" from a moving cursor, which keeps the early
 * allocations contiguous (good for page tables and the heap) while still
 * reusing freed pages.
 */
#include <lume/asm.h>
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/panic.h>
#include <lume/string.h>
#include <lume/types.h>

/* 512 MiB / 4 KiB / 32 bits per word = 4096 words = 16 KiB. */
#define PMM_MAX_PAGES    (512u * 1024u * 1024u / PAGE_SIZE)
#define PMM_BITMAP_WORDS (PMM_MAX_PAGES / 32u)

static u32 bitmap[PMM_BITMAP_WORDS];
static u32 page_base;     /* physical address corresponding to page 0 */
static u32 page_count;    /* pages covered by the bitmap */
static u32 free_pages;    /* pages available for allocation */
static u32 last_alloc;    /* next-fit cursor */

static inline void bit_set(u32 page)
{
    bitmap[page / 32u] |= (1u << (page % 32u));
}

static inline void bit_clear(u32 page)
{
    bitmap[page / 32u] &= ~(1u << (page % 32u));
}

static inline int bit_test(u32 page)
{
    return (bitmap[page / 32u] >> (page % 32u)) & 1u;
}

static inline u32 pa_of_page(u32 page)
{
    return page_base + page * PAGE_SIZE;
}

static inline u32 page_of_pa(u32 pa)
{
    return (pa - page_base) / PAGE_SIZE;
}

void pmm_init(u32 ram_base, u32 ram_end)
{
    u32 pages;

    if (ram_base >= ram_end)
        panic("pmm: invalid RAM range 0x%x..0x%x", ram_base, ram_end);

    ram_base = PAGE_ALIGN_UP(ram_base);
    ram_end = PAGE_ALIGN_DOWN(ram_end);
    pages = (ram_end - ram_base) / PAGE_SIZE;
    if (pages > PMM_MAX_PAGES)
        pages = PMM_MAX_PAGES;

    /* Everything starts free; the arch startup code then reserves the areas
     * that are already in use (firmware data, kernel image, page tables). */
    memset(bitmap, 0, sizeof(u32) * ((pages + 31u) / 32u));
    page_base = ram_base;
    page_count = pages;
    free_pages = pages;
    last_alloc = 0;

    pr_info("pmm: %u MiB RAM at 0x%08x..0x%08x, %u pages",
            (ram_end - ram_base) / (1024 * 1024), ram_base, ram_end, pages);
}

void pmm_reserve(u32 base, u32 size)
{
    u32 first, last;

    if (size == 0)
        return;
    if (base + size <= page_base || base >= page_base + page_count * PAGE_SIZE)
        return;
    if (base < page_base)
        base = page_base;

    first = page_of_pa(PAGE_ALIGN_DOWN(base));
    last = page_of_pa(PAGE_ALIGN_UP(base + size));
    if (last > page_count)
        last = page_count;

    for (u32 page = first; page < last; page++) {
        if (!bit_test(page)) {
            bit_set(page);
            free_pages--;
        }
    }
}

static u32 alloc_from(u32 start, u32 count, u32 align_pages)
{
    u32 page = start;

    if (align_pages < 1)
        align_pages = 1;
    page = (page + align_pages - 1) & ~(align_pages - 1);

    while (page + count <= page_count) {
        u32 i;

        for (i = 0; i < count; i++)
            if (bit_test(page + i))
                break;

        if (i == count) {
            for (i = 0; i < count; i++)
                bit_set(page + i);
            free_pages -= count;
            last_alloc = page + count;
            return page;
        }
        /* Skip past the busy page and re-align. */
        page = page + i + 1;
        page = (page + align_pages - 1) & ~(align_pages - 1);
    }
    return page_count;
}

u32 pmm_alloc_pages(u32 count, u32 align)
{
    u32 flags = arm_irq_save();
    u32 page;

    if (count == 0 || count > free_pages) {
        arm_irq_restore(flags);
        return 0;
    }

    page = alloc_from(last_alloc, count, align / PAGE_SIZE);
    if (page >= page_count)
        page = alloc_from(0, count, align / PAGE_SIZE);

    arm_irq_restore(flags);
    return (page >= page_count) ? 0 : pa_of_page(page);
}

u32 pmm_alloc_page(void)
{
    return pmm_alloc_pages(1, PAGE_SIZE);
}

void pmm_free_pages(u32 pa, u32 count)
{
    u32 flags;
    u32 page;

    if (pa < page_base)
        return;
    page = page_of_pa(pa);
    if (count == 0 || page + count > page_count)
        return;

    flags = arm_irq_save();
    for (u32 i = 0; i < count; i++) {
        if (bit_test(page + i)) {
            bit_clear(page + i);
            free_pages++;
        }
    }
    if (page < last_alloc)
        last_alloc = page;
    arm_irq_restore(flags);
}

void pmm_free_page(u32 pa)
{
    pmm_free_pages(pa, 1);
}

u32 pmm_total_bytes(void)
{
    return page_count * PAGE_SIZE;
}

u32 pmm_free_bytes(void)
{
    return free_pages * PAGE_SIZE;
}

u32 pmm_used_bytes(void)
{
    return (page_count - free_pages) * PAGE_SIZE;
}

u32 pmm_page_count(void)
{
    return page_count;
}
