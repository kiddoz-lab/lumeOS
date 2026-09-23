/*
 * LumeOS init - link layout.
 *
 * The kernel's ELF loader maps PT_LOAD segments at their physical = virtual
 * addresses (a static ET_EXEC image, no relocations, no PIE), so the link
 * address here *is* the address the program runs at.  0x00010000 keeps it well
 * clear of the null page and below the brk heap the kernel places after the
 * image, and matches where a Linux kernel would put a static binary.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x00010000;

    .text : {
        *(.text.start)
        *(.text*)
    }

    .rodata ALIGN(4) : {
        *(.rodata*)
    }

    .data ALIGN(4) : {
        *(.data*)
    }

    .bss ALIGN(4) : {
        *(COMMON)
        *(.bss*)
    }

    /* The build attributes section is deliberately *not* placed here.  Giving
     * it an explicit address (".ARM.attributes 0 : { ... }") makes the linker
     * emit a PT_LOAD segment covering address 0 with a 16 MiB memsz, which the
     * kernel's loader would then try to honour.  Left as an orphan, the linker
     * keeps it as the non-allocated metadata section it is - and the ISA gate
     * still reads the ARMv6 build attributes from it. */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.ARM.exidx*)
    }
}
