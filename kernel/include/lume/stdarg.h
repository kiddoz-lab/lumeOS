/*
 * Minimal freestanding <stdarg.h>.  LumeOS is built with -nostdinc, so the
 * compiler-provided builtins are exposed here rather than relying on a libc
 * header.
 */
#ifndef LUME_STDARG_H
#define LUME_STDARG_H

typedef __builtin_va_list va_list;

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_copy(dst, src)  __builtin_va_copy(dst, src)

#endif /* LUME_STDARG_H */
