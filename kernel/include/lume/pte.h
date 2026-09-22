/*
 * LumeOS page table descriptor definitions (ARMv6 short-descriptor VMSA).
 *
 * Reference: ARM1176JZF-S Technical Reference Manual, chapter 6 "Memory
 * Management Unit" (level one and level two descriptor formats).  ARMv6 is
 * not ARMv7: the level-one section format has an XN bit at bit 4 and no
 * "TEX remap" behaviour, and the level-two small page format uses bit 4 for
 * XN, bit 9 for E (execute-never for the privileged/user distinction is
 * expressed through AP/APX here).
 *
 * These constants are shared by the assembly boot code and the C MMU code so
 * that the two can never disagree.
 */
#ifndef LUME_PTE_H
#define LUME_PTE_H

/* ------------------------------------------------------------------ */
/* Level 1 (section / page-table) descriptors                          */
/* ------------------------------------------------------------------ */
/*
 * Physical memory reserved by the boot stub before the MMU is on.  These are
 * the address the linker script and kernel/arch/arm/boot.S agree on; they are
 * here so that the kernel has a single definition to refer to.
 */
#define BOOT_L1_PA      0x00004000u   /* 16 KiB level 1 translation table */
#define VECTOR_PAGE_PA  0x000F0000u   /* 4 KiB exception vector page, mapped
                                       * at the architectural high-vector
                                       * address 0xFFFF0000 */
#define VECTOR_PAGE_VA  0xFFFF0000u

#define L1_TYPE_MASK 0x3u
#define L1_TYPE_FAULT 0x0u
#define L1_TYPE_PAGETABLE 0x1u  /* pointer to a level 2 table */
#define L1_TYPE_SECTION 0x2u    /* 1 MiB section */

/* Section attribute bits (ARM1176 TRM table 6-2). */
#define L1_SECT_B_BIT     (1u << 2)   /* bufferable */
#define L1_SECT_C_BIT     (1u << 3)   /* cacheable */
#define L1_SECT_XN_BIT    (1u << 4)   /* execute never */
#define L1_SECT_DOMAIN(n) (((n) & 0xFu) << 5)
#define L1_SECT_AP_SHIFT  10
#define L1_SECT_AP_WRITE  (1u << 10)  /* AP[0]: 1 = R/W, 0 = read-only */
#define L1_SECT_AP_USER   (1u << 11)  /* AP[1]: 1 = user accessible */
#define L1_SECT_TEX_SHIFT 12
#define L1_SECT_APX       (1u << 15)  /* AP[2] on ARMv6 (privileged XN) */
#define L1_SECT_SHARABLE  (1u << 16)
#define L1_SECT_NG        (1u << 17)  /* not global */
#define L1_SECT_NS        (1u << 19)  /* not secure (ARMv6 TrustZone) */

/* Convenience: supervisor read/write, executable, cacheable memory.
 * TEX=0b000, C=1, B=1 = Normal memory, write-back, non-shareable. */
#define L1_SECT_KERNEL_ROM ((L1_TYPE_SECTION) | L1_SECT_AP_WRITE | \
                            L1_SECT_DOMAIN(0) | L1_SECT_C_BIT | L1_SECT_B_BIT)
/* Device memory for peripherals: TEX=0b000, C=0, B=1 = Device, shareable. */
#define L1_SECT_DEVICE ((L1_TYPE_SECTION) | L1_SECT_AP_WRITE | \
                        L1_SECT_DOMAIN(0) | L1_SECT_B_BIT | L1_SECT_XN_BIT)

/* ------------------------------------------------------------------ */
/* Level 2 (small page) descriptors                                    */
/* ------------------------------------------------------------------ */
#define L2_TYPE_MASK 0x3u
#define L2_TYPE_FAULT 0x0u
#define L2_TYPE_LARGE_PAGE 0x1u
#define L2_TYPE_SMALL_PAGE 0x2u
#define L2_TYPE_SMALL_PAGE_XN 0x3u  /* same, with XN */

#define L2_SMALL_B_BIT  (1u << 2)
#define L2_SMALL_C_BIT  (1u << 3)
#define L2_SMALL_AP_SHIFT 4
#define L2_SMALL_AP0    (1u << 4)   /* AP[0]: 1 = R/W, 0 = read-only */
#define L2_SMALL_AP1    (1u << 5)   /* AP[1]: 1 = user accessible */
#define L2_SMALL_AP2    (1u << 9)   /* AP[2], ARMv6 (privileged XN) */
#define L2_SMALL_TEX_SHIFT 6
#define L2_SMALL_APX    (1u << 9)
#define L2_SMALL_S      (1u << 10)
#define L2_SMALL_NG     (1u << 11)

/* AP field encodings (ARM1176 TRM table 6-4) for user pages: */
#define L2_AP_USER_RO   (L2_SMALL_AP1)                 /* 0b010: user read-only */
#define L2_AP_USER_RW   (L2_SMALL_AP0 | L2_SMALL_AP1)  /* 0b011: user read/write */

/* Simple classification of a user page. */
#define LUME_PROT_NONE 0
#define LUME_PROT_READ (1u << 0)
#define LUME_PROT_WRITE (1u << 1)
#define LUME_PROT_EXEC (1u << 2)
#define LUME_PROT_USER (1u << 3)  /* user accessible (vs supervisor only) */

#define L2_TABLE_ENTRIES 256
#define L2_TABLE_SIZE 1024u
#define L1_TABLE_ENTRIES 4096
#define L1_TABLE_SIZE 16384u

/* Domain 0 is used for everything (see mmu.c). */
#define LUME_DOMAIN 0u

#endif /* LUME_PTE_H */
