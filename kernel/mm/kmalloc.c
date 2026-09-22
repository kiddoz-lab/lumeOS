/*
 * LumeOS kernel heap.
 *
 * A first-fit allocator with block headers, coalescing on free and a bump
 * region for growth.  The heap lives inside the kernel's linear RAM window
 * (virtual = 0xC0000000 + physical), so growing it is just a matter of asking
 * the page allocator for more pages: no new mappings are required.
 *
 * Design constraints that shaped this code:
 *   - 4 KiB pages, 512 MiB RAM: the allocator must never waste a page for
 *     tiny allocations, hence the header-per-block design with 8-byte
 *     alignment (8-byte alignment keeps 64-bit types and AAPCS happy);
 *   - no dependency on the VFS, threads or interrupts: it must work during
 *     early boot.
 */
#include <lume/asm.h>
#include <lume/config.h>
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/panic.h>
#include <lume/string.h>
#include <lume/types.h>

#define BLOCK_MAGIC 0x4C4D424Bu /* "LMBK" */

struct block {
    u32 magic;
    u32 size;          /* usable bytes (payload), always 8-byte aligned */
    u32 free;
    struct block *next; /* next block in address order */
    struct block *prev; /* previous block in address order */
};

#define HEADER_SIZE (sizeof(struct block))
#define MIN_SPLIT   (HEADER_SIZE + 16)

static struct block *head;
static u32 heap_start_pa;
static u32 heap_end_pa;
static u32 used_bytes;
static u32 total_bytes;

static struct block *block_from_payload(void *payload)
{
    return (struct block *)((u8 *)payload - HEADER_SIZE);
}

static void *payload_from_block(struct block *b)
{
    return (u8 *)b + HEADER_SIZE;
}

static void heap_add_region(u32 pa, u32 size)
{
    u32 va;
    struct block *b, **link;

    size &= ~7u;
    if (size < HEADER_SIZE + 32)
        return;

    va = (u32)PHYS_TO_VIRT(pa);
    b = (struct block *)va;

    b->magic = BLOCK_MAGIC;
    b->size = size - HEADER_SIZE;
    b->free = 1;

    /* Insert in address order: coalescing relies on list neighbours being
     * memory neighbours. */
    link = &head;
    while (*link && (u32)*link < va)
        link = &(*link)->next;

    b->next = *link;
    b->prev = (*link) ? (*link)->prev : NULL;
    if (b->prev)
        b->prev->next = b;
    if (b->next)
        b->next->prev = b;
    *link = b;

    total_bytes += size;
}

/* Grow the heap by at least `size` bytes (rounded up to pages). */
static int heap_grow(u32 size)
{
    u32 pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    u32 pa;

    if (pages < 16)
        pages = 16; /* grow in 64 KiB steps to keep the block list short */

    pa = pmm_alloc_pages(pages, PAGE_SIZE);
    if (!pa)
        return -1;

    if (heap_start_pa == 0)
        heap_start_pa = pa;
    heap_end_pa = pa + pages * PAGE_SIZE;

    heap_add_region(pa, pages * PAGE_SIZE);
    return 0;
}

void kmalloc_init(void)
{
    head = NULL;
    heap_start_pa = 0;
    heap_end_pa = 0;
    used_bytes = 0;
    total_bytes = 0;

    if (heap_grow(LUME_KERNEL_HEAP_INITIAL) < 0)
        panic("kmalloc: cannot allocate the initial kernel heap");

    pr_info("kmalloc: heap at 0x%x..0x%x (%u KiB)", heap_start_pa, heap_end_pa,
            total_bytes / 1024);
}

/* Coalesce `b` with its free neighbours.  Adjacency is verified with pointer
 * arithmetic: the heap can consist of several non-contiguous regions, and
 * merging across a region boundary would corrupt the heap. */
static void coalesce(struct block *b)
{
    struct block *next = b->next;

    if (next && next->free &&
        (u8 *)b + HEADER_SIZE + b->size == (u8 *)next) {
        b->size += HEADER_SIZE + next->size;
        b->next = next->next;
        if (next->next)
            next->next->prev = b;
    }

    struct block *prev = b->prev;

    if (prev && prev->free &&
        (u8 *)prev + HEADER_SIZE + prev->size == (u8 *)b) {
        prev->size += HEADER_SIZE + b->size;
        prev->next = b->next;
        if (b->next)
            b->next->prev = prev;
    }
}

void *kmalloc(u32 size)
{
    struct block *b;
    u32 flags;
    u32 aligned = (size + 7u) & ~7u;

    if (aligned == 0)
        aligned = 8;

    flags = arm_irq_save();
retry:
    for (b = head; b; b = b->next) {
        if (!b->free || b->size < aligned)
            continue;

        /* Split when the remainder can hold another block. */
        if (b->size >= aligned + MIN_SPLIT) {
            struct block *rest = (struct block *)((u8 *)payload_from_block(b) + aligned);

            rest->magic = BLOCK_MAGIC;
            rest->size = b->size - aligned - HEADER_SIZE;
            rest->free = 1;
            rest->next = b->next;
            rest->prev = b;
            if (b->next)
                b->next->prev = rest;
            b->next = rest;
            b->size = aligned;
        }

        b->free = 0;
        used_bytes += b->size + HEADER_SIZE;
        arm_irq_restore(flags);
        return payload_from_block(b);
    }

    if (heap_grow(aligned) == 0)
        goto retry;

    arm_irq_restore(flags);
    pr_err("kmalloc: out of memory (%u bytes requested, %u KiB in use)",
           size, used_bytes / 1024);
    return NULL;
}

void *kzalloc(u32 size)
{
    void *p = kmalloc(size);

    if (p)
        memset(p, 0, size);
    return p;
}

void kfree(void *ptr)
{
    struct block *b;
    u32 flags;

    if (!ptr)
        return;

    b = block_from_payload(ptr);
    if (b->magic != BLOCK_MAGIC) {
        pr_err("kfree: %p is not a heap pointer (magic 0x%x)", ptr, b->magic);
        return;
    }

    flags = arm_irq_save();
    if (b->free) {
        pr_err("kfree: double free of %p", ptr);
    } else {
        b->free = 1;
        used_bytes -= b->size + HEADER_SIZE;
        coalesce(b);
    }
    arm_irq_restore(flags);
}

void *krealloc(void *ptr, u32 size)
{
    struct block *b;
    void *new_ptr;

    if (!ptr)
        return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    b = block_from_payload(ptr);
    if (b->magic != BLOCK_MAGIC)
        return NULL;

    if (b->size >= size)
        return ptr;

    new_ptr = kmalloc(size);
    if (!new_ptr)
        return NULL;
    memcpy(new_ptr, ptr, b->size);
    kfree(ptr);
    return new_ptr;
}

u32 kmalloc_used_bytes(void)
{
    return used_bytes;
}

u32 kmalloc_total_bytes(void)
{
    return total_bytes;
}
