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

/*
 * The program headers are placed *inside* the first PT_LOAD segment, at its
 * very start (FILEHDR and PHDRS are the linker's names for the ELF header and
 * the program headers themselves).  That is what an ordinary Linux ARM binary
 * looks like - GNU ld's default scripts do it - and it matters for one specific
 * reason: a program is told where its own headers are by AT_PHDR, and the
 * kernel computes that address by finding the segment whose *file range*
 * contains them.  If no segment contains them, there is no address to report
 * and AT_PHDR would have to be 0.  Starting the text at 0x10000 + SIZEOF_HEADERS
 * keeps the headers in the segment and costs nothing.
 *
 * The alternative - a segment starting at file offset 0x1000 with the headers
 * left unmapped, which is what this script used to produce - makes the image
 * unusable by any C library, because there is no AT_PHDR to read.
 */
PHDRS
{
    text   PT_LOAD FILEHDR PHDRS FLAGS(5);   /* R E, including the ELF header
                                              * and the program headers */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* RW  */
    stack  PT_GNU_STACK FLAGS(6);  /* "no execute": what a normal binary says
                                    * about its stack.  LumeOS does not use it
                                    * (user stacks are mapped RW, never X, by
                                    * vm_flags), but a program without it is
                                    * asking for an executable stack. */
}

SECTIONS
{
    . = 0x00010000 + SIZEOF_HEADERS;

    .text : {
        *(.text.start)
        *(.text*)
    } :text

    .rodata ALIGN(4) : {
        *(.rodata*)
    } :rodata

    .data ALIGN(4) : {
        *(.data*)
    } :data

    .bss ALIGN(4) : {
        *(COMMON)
        *(.bss*)
    } :data

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
