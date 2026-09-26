/*
 * LumeOS userspace: the auxiliary vector, as a program sees it.
 *
 * This is a deliberate copy of the numbers the kernel writes
 * (kernel/include/lume/auxv.h), for the same reason Linux has both an internal
 * header and a UAPI one: the two sides of an interface are separate programs,
 * and an interface header that is shared cannot catch a disagreement between
 * them.  The program this kernel boots checks the values it reads against these
 * constants, so a drift shows up as a failed boot check rather than as a
 * corrupted data structure three function calls later.
 *
 * Numbers are from the Linux UAPI headers (include/uapi/linux/auxvec.h,
 * arch/arm/include/uapi/asm/hwcap.h).
 */
#ifndef LUME_USER_AUXV_H
#define LUME_USER_AUXV_H

#define LUME_AT_NULL     0
#define LUME_AT_PHDR     3
#define LUME_AT_PHENT    4
#define LUME_AT_PHNUM    5
#define LUME_AT_PAGESZ   6
#define LUME_AT_BASE     7
#define LUME_AT_FLAGS    8
#define LUME_AT_ENTRY    9
#define LUME_AT_UID     11
#define LUME_AT_EUID    12
#define LUME_AT_GID     13
#define LUME_AT_EGID    14
#define LUME_AT_PLATFORM 15
#define LUME_AT_HWCAP   16
#define LUME_AT_CLKTCK  17
#define LUME_AT_SECURE  23
#define LUME_AT_RANDOM  25
#define LUME_AT_HWCAP2  26
#define LUME_AT_EXECFN  31

/* ARM HWCAP bits a program might test (arch/arm/include/uapi/asm/hwcap.h). */
#define LUME_HWCAP_SWP       (1u << 0)
#define LUME_HWCAP_HALF      (1u << 1)
#define LUME_HWCAP_THUMB     (1u << 2)
#define LUME_HWCAP_FAST_MULT (1u << 4)
#define LUME_HWCAP_VFP       (1u << 6)
#define LUME_HWCAP_EDSP      (1u << 7)
#define LUME_HWCAP_NEON      (1u << 12)
#define LUME_HWCAP_TLS       (1u << 15)

#endif /* LUME_USER_AUXV_H */
