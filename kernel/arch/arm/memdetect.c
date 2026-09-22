/*
 * LumeOS memory detection.
 *
 * Three sources, in order of trust:
 *
 *   1. the firmware mailbox property tag GET_ARM_MEMORY (0x00010005), which
 *      reports exactly the region the VideoCore firmware has left for the ARM
 *      core.  This is the authoritative source on a Raspberry Pi.
 *   2. the device tree / ATAGS pointer handed over at boot, which describes
 *      the physical memory; LumeOS does not yet parse the device tree, so it
 *      cannot use this today (see docs/roadmap.md).
 *   3. a conservative fallback for a 512 MiB Raspberry Pi Zero W with the
 *      firmware's default 64 MiB GPU split: ARM-usable RAM ends at 0x1C000000.
 *
 * The fallback exists so that a mailbox that does not answer (or a build
 * running on emulation without the property interface) still boots; the
 * kernel log always says which source was used, and the in-kernel self test
 * reports the numbers userspace sees.
 */
#include <lume/klog.h>
#include <lume/mbox.h>
#include <lume/types.h>

/* BCM2835 ARM peripherals, chapter 2 "Memory":
 * the peripheral/hardware address window starts at 0x20000000 for the ARM. */
#define BCM2835_PERIPHERAL_BASE 0x20000000u
#define BCM2835_PERIPHERAL_SIZE 0x02000000u

/* Physical RAM starts at 0 on a Raspberry Pi Zero W. */
#define PI_RAM_BASE 0x00000000u
/* Default firmware split for a 512 MiB board: 64 MiB for the GPU. */
#define PI_FALLBACK_RAM_END 0x1C000000u

static u32 ram_base;
static u32 ram_end;
static u32 source_is_mailbox;
static u32 fdt_pointer;

void memdetect_set_fdt(u32 fdt_pa)
{
    fdt_pointer = fdt_pa;
}

u32 memdetect_fdt(void)
{
    return fdt_pointer;
}

void memdetect_init(void)
{
    u32 base = 0, size = 0;
    u32 model = 0;

    source_is_mailbox = 0;

    if (mbox_get_arm_memory(&base, &size) == 0 && size >= (32u * 1024u * 1024u)) {
        ram_base = base;
        ram_end = base + size;
        source_is_mailbox = 1;
    } else {
        ram_base = PI_RAM_BASE;
        ram_end = PI_FALLBACK_RAM_END;
        pr_warn("memdetect: firmware did not report ARM memory, "
                "assuming %u MiB starting at 0x%08x",
                (ram_end - ram_base) / (1024 * 1024), ram_base);
    }

    /* The VideoCore memory region is not part of the ARM address space; the
     * ARM alias of the peripherals is reached at 0xF0000000 (see boot.S) and
     * must never overlap RAM. */
    if (ram_end > 0xF0000000u)
        ram_end = 0xF0000000u;

    model = mbox_get_board_model();
    if (model)
        pr_info("memdetect: board model 0x%08x, firmware revision 0x%08x",
                model, mbox_get_firmware_revision());

    pr_info("memdetect: RAM 0x%08x..0x%08x (%u MiB, source: %s)",
            ram_base, ram_end, (ram_end - ram_base) / (1024 * 1024),
            source_is_mailbox ? "firmware mailbox" : "compile-time fallback");
}

u32 memdetect_ram_base(void)
{
    return ram_base;
}

u32 memdetect_ram_end(void)
{
    return ram_end;
}

u32 memdetect_peripheral_base(void)
{
    return BCM2835_PERIPHERAL_BASE;
}

u32 memdetect_peripheral_size(void)
{
    return BCM2835_PERIPHERAL_SIZE;
}
