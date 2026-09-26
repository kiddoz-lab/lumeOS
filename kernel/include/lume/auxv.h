/*
 * The auxiliary vector, and the CPU capabilities this kernel is willing to
 * promise.
 *
 * The auxiliary vector is how a Linux program learns facts about itself that
 * are not in its own image: the page size, where its program headers are, which
 * CPU features it may use, who it is running as.  A static `hello world` reads
 * it before `main` - musl takes AT_PAGESZ and AT_RANDOM, glibc takes nearly
 * everything - which is why it is the first thing after the syscall table that
 * a real binary needs.
 *
 * These numbers are the ABI.  They are *duplicated* in
 * userspace/lib/lume/auxv.h on purpose, exactly as Linux duplicates them
 * between its internal headers and the UAPI ones: the two sides of an
 * interface should be able to be wrong independently, or a typo in a shared
 * header would be invisible.  The program this kernel boots checks the values
 * it reads against its own copy, so a disagreement shows up at boot rather
 * than in a corrupted data structure later.
 *
 * Values are from the Linux UAPI header include/uapi/linux/auxvec.h and, for
 * the HWCAP bits, arch/arm/include/uapi/asm/hwcap.h.
 */
#ifndef LUME_AUXV_H
#define LUME_AUXV_H

#include <lume/types.h>

/* Entry types.  The subset below is what LumeOS writes today; the numbers of
 * the ones it does not write are listed too, because the point of this file is
 * to be compared against the UAPI header. */
#define LUME_AT_NULL     0   /* end of the vector */
#define LUME_AT_IGNORE   1
#define LUME_AT_EXECFD   2
#define LUME_AT_PHDR     3   /* address of the program headers */
#define LUME_AT_PHENT    4   /* size of one program header entry */
#define LUME_AT_PHNUM    5   /* number of program header entries */
#define LUME_AT_PAGESZ   6   /* system page size */
#define LUME_AT_BASE     7   /* base address of the interpreter (0: static) */
#define LUME_AT_FLAGS    8
#define LUME_AT_ENTRY    9   /* entry point */
#define LUME_AT_NOTELF  10
#define LUME_AT_UID     11
#define LUME_AT_EUID    12
#define LUME_AT_GID     13
#define LUME_AT_EGID    14
#define LUME_AT_PLATFORM 15
#define LUME_AT_HWCAP   16
#define LUME_AT_CLKTCK  17
#define LUME_AT_SECURE  23
#define LUME_AT_RANDOM  25   /* 16 bytes for the C library's canary */
#define LUME_AT_HWCAP2  26
#define LUME_AT_EXECFN  31   /* the name the program was started with */

/*
 * ARM HWCAP bits (arch/arm/include/uapi/asm/hwcap.h).  Linux's table for an
 * ARMv6 core - arch/arm/mm/proc-v6.S - is
 *
 *     HWCAP_SWP | HWCAP_HALF | HWCAP_THUMB | HWCAP_FAST_MULT | HWCAP_EDSP |
 *     HWCAP_JAVA | HWCAP_TLS
 *
 * and that is what this file would report for an ARM1176JZF-S, with two
 * deliberate differences, both stated in docs/userspace.md:
 *
 *   - HWCAP_JAVA is *not* set.  The core has Jazelle DBX, but this kernel never
 *     enables the Jazelle state (it would have to enter and leave it around
 *     every switch), so advertising it would invite a JVM to issue instructions
 *     the kernel is not prepared to handle.
 *   - no VFP/NEON/VFPv3/VFPv4/FPA bit is ever set.  The BCM2835's ARM1176 does
 *     have VFPv2, and Linux would set HWCAP_VFP - but Linux also saves and
 *     restores the FP registers across a context switch, and LumeOS does not.
 *     A program that took HWCAP_VFP at its word would find its floating point
 *     state clobbered by any other thread that ran in between.  So the promise
 *     is not made, and a soft-float build is the only supported one until the
 *     kernel grows VFP state saving (docs/roadmap.md, milestone 3).
 *
 * HWCAP_THUMB is honest about the hardware but *unproven* in this kernel: no
 * program has been built with -mthumb yet, so the exception path's interworking
 * is untested - reported as a gap in docs/testing.md rather than quietly
 * claimed.
 */
#define LUME_HWCAP_SWP       (1u << 0)   /* swap/swpb */
#define LUME_HWCAP_HALF      (1u << 1)   /* halfword and signed-byte loads */
#define LUME_HWCAP_THUMB     (1u << 2)   /* 16-bit Thumb instructions */
#define LUME_HWCAP_FAST_MULT (1u << 4)
#define LUME_HWCAP_EDSP      (1u << 7)   /* the DSP (saturating) instructions */
#define LUME_HWCAP_JAVA      (1u << 8)   /* Jazelle - see above: not set */
#define LUME_HWCAP_TLS       (1u << 15)  /* TPIDRURO is usable for TLS */

/* Named here even though it is *never* set, because "we do not advertise VFP"
 * is a decision this kernel makes, not an oversight - and a decision is easier
 * to check (the self test asserts the bit is clear) than an absence.  Same for
 * the other floating-point capability bits. */
#define LUME_HWCAP_FPA       (1u << 5)
#define LUME_HWCAP_VFP       (1u << 6)
#define LUME_HWCAP_NEON      (1u << 12)
#define LUME_HWCAP_VFPv3     (1u << 13)
#define LUME_HWCAP_VFPv4     (1u << 16)
#define LUME_HWCAP_FP_MASK \
    (LUME_HWCAP_FPA | LUME_HWCAP_VFP | LUME_HWCAP_NEON | \
     LUME_HWCAP_VFPv3 | LUME_HWCAP_VFPv4)

#define LUME_HWCAP_ARMv6KZ \
    (LUME_HWCAP_SWP | LUME_HWCAP_HALF | LUME_HWCAP_THUMB | \
     LUME_HWCAP_FAST_MULT | LUME_HWCAP_EDSP | LUME_HWCAP_TLS)

/* The value AT_HWCAP carries, with the proof that nothing floating-point crept
 * in: a build-time check of the constant itself, so the promise cannot be
 * widened by editing the list above without failing to compile. */
STATIC_ASSERT((LUME_HWCAP_ARMv6KZ & LUME_HWCAP_FP_MASK) == 0,
              "LumeOS must not advertise floating point: it does not save FP "
              "state across a context switch");

/*
 * AT_PLATFORM.  Linux builds this from the CPU's `elf_name` plus an endianness
 * letter, and arch/arm/mm/proc-v6.S says `v6` for every ARMv6 core, so an
 * ARM1176JZF-S reports "v6l" - the K and Z extensions have no separate
 * platform string.  It is used for /lib/<platform>/ hwcap subdirectories.
 */
#define LUME_ELF_PLATFORM "v6l"

/* AT_CLKTCK is the frequency times(2) counts in: Linux reports the fixed
 * USER_HZ of 100 regardless of the kernel's own HZ, and a program that calls
 * times() or sysconf(_SC_CLK_TCK) believes it.  LumeOS's scheduler tick is
 * also 100 Hz (LUME_HZ), which makes the two agree by construction. */
#define LUME_AT_CLKTCK_VALUE 100

#endif /* LUME_AUXV_H */
