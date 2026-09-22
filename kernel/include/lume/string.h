/*
 * LumeOS freestanding C string/memory routines.
 *
 * Because -nostdinc is used, these are the only implementations available to
 * the kernel.  They are written to avoid unaligned word accesses: LumeOS runs
 * with SCTLR.A=1 (alignment checking on), so an unaligned load is a fault.
 */
#ifndef LUME_STRING_H
#define LUME_STRING_H

#include <lume/types.h>

void *memset(void *dst, int c, u32 n);
void *memcpy(void *dst, const void *src, u32 n);
void *memmove(void *dst, const void *src, u32 n);
int   memcmp(const void *a, const void *b, u32 n);
void *memchr(const void *s, int c, u32 n);
u32   memzero(void *dst, u32 n);

u32   strlen(const char *s);
u32   strnlen(const char *s, u32 max);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, u32 n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, u32 n);
char *strcat(char *dst, const char *src);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

/* Parse an unsigned integer; returns the number of characters consumed. */
u32 strtoul(const char *s, u32 *out, u32 base);
int strtol_signed(const char *s, s32 *out, u32 base);

char *utoa(u32 value, char *buf, u32 base);
char *itoa(s32 value, char *buf, u32 base);

#endif /* LUME_STRING_H */
