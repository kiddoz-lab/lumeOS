/*
 * LumeOS compiler/attribute helpers.
 */
#ifndef LUME_COMPILER_H
#define LUME_COMPILER_H

#include <lume/types.h>

#define __init      __attribute__((section(".text.init")))
#define __unused    __attribute__((unused))
#define __used      __attribute__((used))
#define __packed    __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define __noreturn  __attribute__((noreturn))
#define __weak      __attribute__((weak))
#define __must_check __attribute__((warn_unused_result))
#define __printf(a, b) __attribute__((format(printf, a, b)))

#define barrier() asm volatile("" ::: "memory")

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

#define offsetof(t, m) __builtin_offsetof(t, m)

/* Rounding helpers for integers (the macro versions in types.h work on any
 * integer expression, these require power-of-two alignment). */
static inline u32 round_up(u32 v, u32 align)
{
    return (v + align - 1) & ~(align - 1);
}

static inline u32 round_down(u32 v, u32 align)
{
    return v & ~(align - 1);
}

/* Unreachable code marker used in switch statements over ABI enums. */
#define unreachable() __builtin_unreachable()

#endif /* LUME_COMPILER_H */
