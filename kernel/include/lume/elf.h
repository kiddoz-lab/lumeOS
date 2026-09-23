/*
 * LumeOS ELF32 loader for static ARM Linux executables.
 *
 * The target is exactly the "ET_EXEC, EM_ARM, ELFCLASS32, little-endian,
 * no relocations" case, which is what a compiler produces for
 * "-static -nostdlib" and what a static musl/glibc build produces as well
 * (those additionally need a dynamic linker only when they are PIE; a static
 * non-PIE binary is ET_EXEC like this one).
 *
 * Deliberately out of scope, and refused rather than mis-loaded:
 *
 *   - ET_DYN/shared objects and PIE (they need relocations applied by a
 *     loader that understands them; a static ET_EXEC needs none),
 *   - any architecture other than EM_ARM,
 *   - segments whose addresses are outside the user address range, because a
 *     crafted image must not be able to map kernel memory into user space.
 *
 * The structure definitions below are the ones from the ELF specification
 * (ELF32, section 1) and are used as written; the static asserts exist because
 * a wrong struct layout here would silently load garbage.
 */
#ifndef LUME_ELF_H
#define LUME_ELF_H

#include <lume/mem.h>
#include <lume/types.h>

#define ELF_EI_CLASS   4
#define ELF_EI_DATA    5
#define ELFCLASS32     1
#define ELFDATA2LSB    1
#define ELF_VERSION_CURRENT 1

#define ELF_ET_EXEC    2
#define ELF_ET_DYN     3
#define ELF_EM_ARM     40

#define ELF_PT_LOAD    1

#define ELF_PF_X       1
#define ELF_PF_W       2
#define ELF_PF_R       4

struct elf32_ehdr {
    u8  e_ident[16];
    u16 e_type;
    u16 e_machine;
    u32 e_version;
    u32 e_entry;
    u32 e_phoff;
    u32 e_shoff;
    u32 e_flags;
    u16 e_ehsize;
    u16 e_phentsize;
    u16 e_phnum;
    u16 e_shentsize;
    u16 e_shnum;
    u16 e_shstrndx;
};

struct elf32_phdr {
    u32 p_type;
    u32 p_offset;
    u32 p_vaddr;
    u32 p_paddr;
    u32 p_filesz;
    u32 p_memsz;
    u32 p_flags;
    u32 p_align;
};

STATIC_ASSERT(sizeof(struct elf32_ehdr) == 52, "ELF32 header layout");
STATIC_ASSERT(sizeof(struct elf32_phdr) == 32, "ELF32 program header layout");

/** What a successful load produced. */
struct elf_image {
    u32 entry;        /* e_entry: where user mode starts */
    u32 image_end;    /* first address past the image, page-aligned */
    u32 segments;     /* PT_LOAD segments mapped */
    u32 pages;        /* pages mapped (image only, not the stack) */
};

/*
 * Validate the headers of an ELF image without mapping anything.  Returns 0 and
 * fills *ehdr_out on success, -1 with a reason printed by the caller otherwise.
 * `size` is the number of bytes the image is known to occupy; every offset in
 * the headers is checked against it, because a truncated image must fail here
 * rather than fault the loader in the middle of a copy.
 */
int elf_validate(const u8 *image, u32 size, const struct elf32_ehdr **ehdr_out,
                 const char **reason_out);

/*
 * Load an already-validated image into `as`: allocate and map pages for every
 * PT_LOAD segment, copy the file contents, zero the remainder (bss), flush the
 * caches so the new code is executable, and report the entry point.
 *
 * On failure every page it mapped is freed and the address space is left as it
 * was, so the caller can destroy it without leaking (or use it for something
 * else, which is what the kernel does at boot).
 */
int elf_load(struct vm_space *as, const u8 *image, u32 size,
             struct elf_image *out);

#endif /* LUME_ELF_H */
