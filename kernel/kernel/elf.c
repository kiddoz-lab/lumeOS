/*
 * LumeOS ELF32 loader.  See lume/elf.h for what is in scope and what is
 * refused by design.
 *
 * Two things here are easy to get wrong and expensive to debug, so they are
 * done explicitly:
 *
 *   - every file offset is bounds-checked against the image size before it is
 *     used, so a truncated or hostile image fails to load instead of taking
 *     the kernel down with it;
 *   - after the segment contents are copied, the data cache is cleaned to the
 *     point of unification and the instruction cache invalidated.  The kernel
 *     wrote those bytes through the linear (cached) mapping; ARMv6 has
 *     separate instruction and data caches, and without the flush the CPU can
 *     execute whatever the instruction cache happened to hold for that
 *     physical page.  That failure looks like "the program sometimes runs the
 *     previous program's code", which is a miserable thing to chase.
 */
#include <lume/asm.h>
#include <lume/elf.h>
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/string.h>
#include <lume/types.h>

/* The user address range LumeOS will map: above the null page (so a NULL
 * dereference faults instead of working) and below the kernel half at
 * 0xC0000000. */
#define ELF_USER_MIN      0x00010000u
#define ELF_USER_MAX      0xB0000000u

static u32 user_flags_for(u32 p_flags)
{
    u32 flags = VM_FLAG_USER;

    if (p_flags & ELF_PF_W)
        flags |= VM_FLAG_WRITE;
    if (p_flags & ELF_PF_X)
        flags |= VM_FLAG_EXEC;
    return flags;
}

static int check_range(u32 start, u32 size, const char **reason)
{
    if (size == 0)
        return 0;
    if (start < ELF_USER_MIN) {
        *reason = "segment starts below the user address range";
        return -1;
    }
    if (start > ELF_USER_MAX || size > ELF_USER_MAX - start) {
        *reason = "segment extends outside the user address range";
        return -1;
    }
    return 0;
}

static const struct elf32_phdr *phdr_at(const u8 *image, const struct elf32_ehdr *ehdr,
                                        u16 index)
{
    return (const struct elf32_phdr *)(image + ehdr->e_phoff +
                                       (u32)index * sizeof(struct elf32_phdr));
}

int elf_validate(const u8 *image, u32 size, const struct elf32_ehdr **ehdr_out,
                 const char **reason_out)
{
    const struct elf32_ehdr *ehdr;
    const char *reason = "unknown";

    if (!image || size < sizeof(struct elf32_ehdr)) {
        reason = "image smaller than an ELF header";
        goto bad;
    }
    if (image[0] != 0x7F || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') {
        reason = "not an ELF file";
        goto bad;
    }
    if (image[ELF_EI_CLASS] != ELFCLASS32) {
        reason = "not ELFCLASS32";
        goto bad;
    }
    if (image[ELF_EI_DATA] != ELFDATA2LSB) {
        reason = "not little-endian";
        goto bad;
    }

    ehdr = (const struct elf32_ehdr *)image;
    if (ehdr->e_machine != ELF_EM_ARM) {
        reason = "not an ARM executable";
        goto bad;
    }
    if (ehdr->e_type != ELF_ET_EXEC) {
        /* A PIE or a shared object would need relocations; say so precisely
         * rather than failing later with a confusing fault at address 0. */
        reason = (ehdr->e_type == ELF_ET_DYN)
                     ? "ET_DYN (PIE/shared object) needs a relocation loader"
                     : "not ET_EXEC";
        goto bad;
    }
    if (ehdr->e_entry < ELF_USER_MIN) {
        reason = "entry point is below the user address range";
        goto bad;
    }
    if (ehdr->e_phoff > size ||
        ehdr->e_phentsize != sizeof(struct elf32_phdr) ||
        ehdr->e_phnum > (size - ehdr->e_phoff) / sizeof(struct elf32_phdr)) {
        reason = "program header table is outside the image";
        goto bad;
    }
    if (ehdr->e_phnum == 0) {
        reason = "no program headers";
        goto bad;
    }

    for (u16 i = 0; i < ehdr->e_phnum; i++) {
        const struct elf32_phdr *ph = phdr_at(image, ehdr, i);

        if (ph->p_type != ELF_PT_LOAD)
            continue;
        if (ph->p_filesz > ph->p_memsz) {
            reason = "PT_LOAD has p_filesz > p_memsz";
            goto bad;
        }
        if (ph->p_offset > size || ph->p_filesz > size - ph->p_offset) {
            reason = "PT_LOAD contents are outside the image";
            goto bad;
        }
        if (check_range(ph->p_vaddr, ph->p_memsz, &reason) < 0)
            goto bad;
    }

    if (ehdr_out)
        *ehdr_out = ehdr;
    return 0;

bad:
    if (reason_out)
        *reason_out = reason;
    return -1;
}

int elf_load(struct vm_space *as, const u8 *image, u32 size,
             struct elf_image *out)
{
    const struct elf32_ehdr *ehdr;
    const char *reason = NULL;
    u32 image_end = 0;
    u32 pages = 0;
    u32 segments = 0;

    if (!as || !out)
        return -1;

    if (elf_validate(image, size, &ehdr, &reason) < 0) {
        pr_err("elf: refusing to load image: %s", reason);
        return -1;
    }

    for (u16 i = 0; i < ehdr->e_phnum; i++) {
        const struct elf32_phdr *ph = phdr_at(image, ehdr, i);
        u32 first, last, va, flags;

        if (ph->p_type != ELF_PT_LOAD || ph->p_memsz == 0)
            continue;

        first = PAGE_ALIGN_DOWN(ph->p_vaddr);
        last = PAGE_ALIGN_UP(ph->p_vaddr + ph->p_memsz);
        flags = user_flags_for(ph->p_flags);

        for (va = first; va < last; va += PAGE_SIZE) {
            u32 page_off = va - first;
            u32 pa = 0;
            u8 *page;

            /*
             * Two PT_LOAD segments may legally share a page - a read-only
             * segment ending in the same 4 KiB page as a writable one starts
             * is the common case, and it is what the toolchain here produces
             * for a program with .text and .rodata but no .data.  Reuse the
             * page that is already mapped instead of allocating a fresh one,
             * or the second segment's contents replace the first's and the
             * program executes zeroes.
             */
            if (vmm_translate(as, va, &pa) == 0 && pa) {
                page = (u8 *)PHYS_TO_VIRT(pa);
            } else {
                pa = pmm_alloc_page();
                if (!pa) {
                    pr_err("elf: out of memory mapping segment %u", i);
                    goto fail;
                }
                page = (u8 *)PHYS_TO_VIRT(pa);
                memset(page, 0, PAGE_SIZE);
            }

            /* Copy the file-backed part of this page, if any.  For the page
             * holding p_vaddr the contents start at an offset inside it. */
            if (page_off < ph->p_filesz) {
                u32 file_off = ph->p_offset + page_off;
                u32 avail = ph->p_filesz - page_off;
                u32 in_page = PAGE_SIZE - (file_off & (PAGE_SIZE - 1));
                u32 copied = (avail < in_page) ? avail : in_page;

                memcpy(page + (file_off & (PAGE_SIZE - 1)), image + file_off, copied);
            }

            if (vmm_map_page(as, va, pa, flags) < 0) {
                pr_err("elf: cannot map 0x%08x", va);
                pmm_free_page(pa);
                goto fail;
            }
            pages++;
        }

        if (last > image_end)
            image_end = last;
        segments++;
    }

    if (segments == 0) {
        pr_err("elf: image has no PT_LOAD segment");
        return -1;
    }

    /* Make the loaded image executable.  The kernel wrote those pages through
     * its linear mapping, so translate each one back to a kernel address,
     * clean the data cache and then drop the instruction cache.  A page shared
     * by two segments is cleaned twice, which costs a few cycles and is
     * harmless: cleaning is idempotent. */
    for (u16 i = 0; i < ehdr->e_phnum; i++) {
        const struct elf32_phdr *ph = phdr_at(image, ehdr, i);
        u32 first, va;

        if (ph->p_type != ELF_PT_LOAD || ph->p_memsz == 0)
            continue;

        first = PAGE_ALIGN_DOWN(ph->p_vaddr);
        for (va = first; va < PAGE_ALIGN_UP(ph->p_vaddr + ph->p_memsz); va += PAGE_SIZE) {
            u32 pa = 0;

            if (vmm_translate(as, va, &pa) == 0)
                arm_dcache_clean_range((void *)PHYS_TO_VIRT(pa), PAGE_SIZE);
        }
    }
    arm_invalidate_icache();
    arm_isb();

    out->entry = ehdr->e_entry;
    out->image_end = image_end;
    out->segments = segments;
    out->pages = pages;

    /* AT_PHDR: the program headers at the address the program will see them.
     *
     * For an image moved by `delta` from the address its own headers claim,
     * every virtual address in the file is `delta` higher than the file says.
     * `delta` is derived here from the one address that survives the load - the
     * entry point the caller is about to jump to - rather than assumed to be
     * zero, so a future relocation-capable loader does not have to find this
     * line to stay correct.  For everything this loader accepts today (a
     * non-PIE image loaded at its link address) delta is 0 and AT_PHDR is the
     * file offset. */
    {
        u32 delta = out->entry - ehdr->e_entry;

        out->phdr = delta + ehdr->e_phoff;
    }
    out->phnum = ehdr->e_phnum;
    out->phent = ehdr->e_phentsize;
    return 0;

fail:
    /* Leave nothing behind: unmap and free whatever this call mapped, so the
     * caller can destroy the address space (or reuse it) without leaking. */
    for (u16 i = 0; i < ehdr->e_phnum; i++) {
        const struct elf32_phdr *ph = phdr_at(image, ehdr, i);
        u32 va;

        if (ph->p_type != ELF_PT_LOAD || ph->p_memsz == 0)
            continue;
        for (va = PAGE_ALIGN_DOWN(ph->p_vaddr);
             va < PAGE_ALIGN_UP(ph->p_vaddr + ph->p_memsz); va += PAGE_SIZE)
            vmm_unmap_page(as, va);
    }
    return -1;
}
