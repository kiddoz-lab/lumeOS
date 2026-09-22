#ifndef LUME_PANIC_H
#define LUME_PANIC_H

#include <lume/compiler.h>
#include <lume/types.h>

/** Print a message and halt the kernel forever. */
void panic(const char *fmt, ...) __printf(1, 2) __noreturn;

/** Panic helper callable from assembly (no formatting). */
void panic_early(const char *msg, u32 lr) __noreturn;

/** Non-zero once panic() has been entered (used by tests/diagnostics). */
int panic_pending(void);

/** Reset the SoC through the watchdog block.  Never returns. */
void lume_reboot(void) __noreturn;

/** Halt the CPU with interrupts disabled.  Never returns. */
void lume_poweroff(void) __noreturn;

#define BUG() panic("BUG at %s:%d in %s()", __FILE__, __LINE__, __func__)
#define BUG_ON(cond) do { if (unlikely(cond)) BUG(); } while (0)

#endif /* LUME_PANIC_H */
