/*
 * LumeOS base types and compile-time helpers.
 */
#ifndef LUME_TYPES_H
#define LUME_TYPES_H

typedef unsigned char      u8;
typedef signed char        s8;
typedef unsigned short     u16;
typedef signed short       s16;
typedef unsigned int       u32;
typedef signed int         s32;
typedef unsigned long long u64;
typedef signed long long   s64;

/*
 * Integer type wide enough to hold a pointer.  __UINTPTR_TYPE__ is predeclared
 * by GCC and Clang: on ARMv6 it is `unsigned int` (32 bits), exactly what the
 * kernel wants.  Using the builtin instead of a hard-coded u32 keeps the
 * host-side unit tests (tests/host, compiled for x86-64) honest, because there
 * a u32 would silently truncate every pointer they hand back to the test.
 */
typedef __UINTPTR_TYPE__ uintptr_t_lume;

typedef u32 ssize_t_lume;

#define NULL ((void *)0)

/* Container size helpers. */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define __stringify(x) #x
#define stringify(x) __stringify(x)

#define BIT(n) (1u << (n))
#define ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((typeof(x))(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((typeof(x))(a) - 1))
#define IS_ALIGNED(x, a) (((x) & ((a) - 1)) == 0)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* Compile-time assertion (works in C11 without <assert.h>). */
#define BUILD_BUG_ON(cond) _Static_assert(!(cond), "build bug: " #cond)
#define STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

/* Page size: ARMv6 with the standard short-descriptor format supports 4 KiB
 * small pages, which is the granularity LumeOS uses everywhere. */
#define PAGE_SHIFT 12
#define PAGE_SIZE  (1u << PAGE_SHIFT)
#define PAGE_MASK  (PAGE_SIZE - 1)
#define PAGE_ALIGN_UP(x)   ALIGN_UP((u32)(x), PAGE_SIZE)
#define PAGE_ALIGN_DOWN(x) ALIGN_DOWN((u32)(x), PAGE_SIZE)

/* 1 MiB sections: the granularity used for kernel/device mappings. */
#define SECTION_SHIFT 20
#define SECTION_SIZE  (1u << SECTION_SHIFT)

/* Address space layout.  The kernel is mapped at 0xC0000000 + physical for
 * the low 1 GiB of RAM (identical to the layout used by ARM Linux, which
 * makes reasoning about the ABI layer easier), user space lives below
 * 0xC0000000. */
#define KERNEL_VBASE     0xC0000000u
/* RAM alias only.  Peripheral registers need PERIPHERAL_TO_VIRT() from
 * <lume/hw/bcm2835.h>; they live in a separate virtual window. */
#define PHYS_TO_VIRT(p)  ((void *)((u32)(p) + KERNEL_VBASE))
#define VIRT_TO_PHYS(v)  ((u32)(v) - KERNEL_VBASE)
#define KERNEL_ADDR(p)   PHYS_TO_VIRT(p)
#define USER_TOP         0xC0000000u

#endif /* LUME_TYPES_H */
