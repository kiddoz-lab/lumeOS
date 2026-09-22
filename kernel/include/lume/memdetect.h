/*
 * LumeOS memory and board detection interface.
 *
 * The implementation is BCM2835-specific (arch/arm/memdetect.c): it asks the
 * VideoCore firmware what RAM the ARM core may use and falls back to a
 * documented default when the firmware does not answer.
 */
#ifndef LUME_MEMDETECT_H
#define LUME_MEMDETECT_H

#include <lume/types.h>

/** Remember the firmware-provided ATAGS/device tree pointer (from boot.S). */
void memdetect_set_fdt(u32 fdt_pa);
u32  memdetect_fdt(void);

/** Detect the ARM-usable RAM range.  Must be called after the mailbox works. */
void memdetect_init(void);

u32 memdetect_ram_base(void);
u32 memdetect_ram_end(void);

/** The ARM view of the peripheral window (0x20000000 on BCM2835). */
u32 memdetect_peripheral_base(void);
u32 memdetect_peripheral_size(void);

#endif /* LUME_MEMDETECT_H */
