/*
 * LumeOS kernel linker script (BCM2835 / Raspberry Pi Zero W, ARMv6)
 *
 * The Raspberry Pi boot firmware loads a flat binary image at physical address
 * 0x8000 and jumps to its first byte (see docs/boot-pi.md and
 * docs/research-notes.md for the sources of these facts).  LumeOS therefore
 * links its kernel to run from a high virtual alias (0xC0000000 + physical)
 * while still being *loaded* at physical 0x8000.  The early boot stub in
 * arch/arm/boot.S is position independent, builds the initial translation
 * tables, enables the MMU and branches to the high alias.
 *
 * The single PHDRS statement below is not decoration: without it the linker
 * creates an extra PT_LOAD segment covering the ELF header at the virtual
 * address (0xC0000000), which makes a naive ELF-to-flat-binary conversion
 * produce a ~3 GiB file.  One PT_LOAD segment keeps the loadable image
 * contiguous and lets tools/elf2bin.py - which enforces the physical load
 * address - stay simple.
 *
 * A 4 KiB page at physical 0x000F0000 is reserved for the exception vector
 * page.  It is pinned to that physical address because it is mapped as part of
 * a 1 MiB section at virtual 0xFFFF0000 (the architectural high-vector
 * address) by the boot code, before any allocator exists.  The kernel image
 * must therefore end below 0xF0000, which the ASSERT below enforces.
 */

KERNEL_LMA_BASE = 0x00008000;
KERNEL_VMA_BASE = 0xC0000000;
KERNEL_VIRT     = KERNEL_VMA_BASE + KERNEL_LMA_BASE;

/* Physical address of the exception vector page (see comment above). */
VECTOR_PAGE_PA  = 0x000F0000;

ENTRY(_start)

PHDRS
{
    image PT_LOAD FLAGS(7);   /* R W X: the firmware does not care, we do */
}

SECTIONS
{
    . = KERNEL_VIRT;

    /* The boot stub must be first so that it sits at physical 0x8000. */
    .text : AT(KERNEL_LMA_BASE) {
        KEEP(*(.text.boot))
        *(.text .text.*)
    } :image

    .rodata ALIGN(8) : {
        *(.rodata .rodata.*)
    } :image

    .data ALIGN(8) : {
        _data_start = .;
        *(.data .data.*)
        _data_end = .;
    } :image

    .bss ALIGN(256) (NOLOAD) : {
        . = ALIGN(8);
        __bss_start = .;
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4096);
        __bss_end = .;
        _bss_end = .;
    } :image

    . = ALIGN(4096);
    ASSERT(. <= KERNEL_VMA_BASE + VECTOR_PAGE_PA,
           "LumeOS kernel image is too large: it would overlap the exception vector page at physical 0xF0000")

    /* End of the loaded image, in both virtual and physical terms. */
    _kernel_image_end    = .;
    _kernel_image_end_pa = ABSOLUTE(. - KERNEL_VMA_BASE);

    /DISCARD/ : {
        *(.comment)
        *(.ARM.exidx*)
        *(.note.GNU-stack)
        *(.eh_frame*)
    }
}
