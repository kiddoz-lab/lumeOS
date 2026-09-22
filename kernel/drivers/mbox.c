/*
 * LumeOS Raspberry Pi firmware mailbox driver.
 *
 * Message format and tag identifiers: Raspberry Pi firmware wiki, "Mailbox
 * property interface".  Hardware registers: BCM2835 ARM Peripherals chapter
 * 13 ("Mailboxes"), mailbox 0 for ARM->VC traffic, channel 8 for the property
 * interface.
 *
 * Cache coherency: the VideoCore does not snoop the ARM data cache, so the
 * message buffer is cleaned before the request and invalidated after the
 * response.  This is the "DMA coherent buffer" pattern without needing a
 * dedicated uncached mapping for a single buffer.
 */
#include <lume/asm.h>
#include <lume/hw/bcm2835.h>
#include <lume/klog.h>
#include <lume/mbox.h>
#include <lume/string.h>
#include <lume/types.h>

#define MBOX_REG(off) (*(volatile u32 *)((u32)PHYS_TO_VIRT(BCM2835_MBOX_BASE) + (off)))

/* Property message buffer: 16-byte aligned as required by the protocol. */
#define PROP_BUFFER_WORDS 64
static u32 prop_buffer[PROP_BUFFER_WORDS] __aligned(16);
static u32 prop_index;      /* next free word index inside prop_buffer */
static int prop_open;       /* a message is being built */

void mbox_init(void)
{
    prop_index = 0;
    prop_open = 0;
}

int mbox_call(u32 *msg, u32 channel)
{
    u32 addr = VIRT_TO_PHYS(msg);
    u32 flags;
    u32 tries;

    if (addr & 0xFu) {
        pr_err("mbox: message buffer %p is not 16-byte aligned", msg);
        return -1;
    }

    flags = arm_irq_save();

    /* Clean the request out of the data cache so the VideoCore sees it. */
    arm_dcache_clean_invalidate_range(msg, msg[0]);

    /* Wait for space in the mailbox. */
    for (tries = 0; MBOX_REG(MBOX_STATUS) & MBOX_STATUS_FULL; tries++) {
        if (tries > 1000000u) {
            arm_irq_restore(flags);
            pr_err("mbox: mailbox never became ready (status=0x%x)", MBOX_REG(MBOX_STATUS));
            return -1;
        }
    }

    arm_dsb();
    MBOX_REG(MBOX_WRITE) = (addr & ~0xFu) | (channel & 0xFu);
    arm_dsb();

    /* Wait for our response: the firmware echoes the message back on the same
     * channel.  Other channels' traffic is simply dropped. */
    for (tries = 0; tries < 10000000u; tries++) {
        if (MBOX_REG(MBOX_STATUS) & MBOX_STATUS_EMPTY)
            continue;
        u32 response = MBOX_REG(MBOX_READ);
        if ((response & 0xFu) == (channel & 0xFu)) {
            /* The firmware overwrote the buffer: invalidate stale lines. */
            arm_dcache_invalidate_all();
            arm_irq_restore(flags);
            return 0;
        }
    }

    arm_irq_restore(flags);
    pr_err("mbox: no response from the VideoCore firmware");
    return -1;
}

/* ------------------------------------------------------------------ */
/* Property message builder                                            */
/* ------------------------------------------------------------------ */

void mbox_prop_begin(void)
{
    prop_index = 2;
    prop_open = 1;
    prop_buffer[0] = 0;
    prop_buffer[1] = 0;
}

int mbox_prop_add(u32 tag, u32 value_bytes, const u32 *values)
{
    u32 words = (value_bytes + 3) / 4;
    u32 i;

    if (!prop_open || prop_index + 3 + words + 1 > PROP_BUFFER_WORDS)
        return -1;

    prop_buffer[prop_index++] = tag;
    prop_buffer[prop_index++] = value_bytes;
    prop_buffer[prop_index++] = 0; /* request: response length 0 */
    for (i = 0; i < words; i++)
        prop_buffer[prop_index++] = values ? values[i] : 0;
    return 0;
}

int mbox_prop_send(void)
{
    int ret;

    if (!prop_open)
        return -1;

    prop_buffer[prop_index] = 0; /* end tag */
    /* Total size includes the size word itself, rounded up to 16 bytes. */
    prop_buffer[0] = (prop_index + 1) * 4;
    prop_buffer[0] = (prop_buffer[0] + 15u) & ~15u;

    ret = mbox_call(prop_buffer, MBOX_CH_PROPERTY);
    if (ret == 0 && prop_buffer[1] != MBOX_STATUS_SUCCESS)
        ret = -1;

    prop_open = 0;
    return ret;
}

u32 *mbox_prop_get(u32 tag)
{
    u32 i = 2;

    while (i < (prop_buffer[0] / 4)) {
        u32 id = prop_buffer[i];
        u32 size = prop_buffer[i + 1];
        u32 words = (size + 3) / 4;

        if (id == 0)
            break;
        if (id == tag)
            return &prop_buffer[i + 3];
        i += 3 + words;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Convenience wrappers                                                */
/* ------------------------------------------------------------------ */

u32 mbox_get_clock_rate(u32 clock_id)
{
    u32 in[2] = { clock_id, 0 };
    u32 *out;

    mbox_prop_begin();
    if (mbox_prop_add(MBOX_TAG_GET_CLOCK_RATE, sizeof(in), in) < 0)
        return 0;
    if (mbox_prop_send() < 0)
        return 0;
    out = mbox_prop_get(MBOX_TAG_GET_CLOCK_RATE);
    return out ? out[1] : 0;
}

u32 mbox_get_max_clock_rate(u32 clock_id)
{
    u32 in[2] = { clock_id, 0 };
    u32 *out;

    mbox_prop_begin();
    if (mbox_prop_add(MBOX_TAG_GET_MAX_CLOCK_RATE, sizeof(in), in) < 0)
        return 0;
    if (mbox_prop_send() < 0)
        return 0;
    out = mbox_prop_get(MBOX_TAG_GET_MAX_CLOCK_RATE);
    return out ? out[1] : 0;
}

u32 mbox_get_firmware_revision(void)
{
    u32 in = 0;
    u32 *out;

    mbox_prop_begin();
    if (mbox_prop_add(MBOX_TAG_GET_FIRMWARE_REVISION, sizeof(in), &in) < 0)
        return 0;
    if (mbox_prop_send() < 0)
        return 0;
    out = mbox_prop_get(MBOX_TAG_GET_FIRMWARE_REVISION);
    return out ? out[0] : 0;
}

u32 mbox_get_board_model(void)
{
    u32 in = 0;
    u32 *out;

    mbox_prop_begin();
    if (mbox_prop_add(MBOX_TAG_GET_BOARD_MODEL, sizeof(in), &in) < 0)
        return 0;
    if (mbox_prop_send() < 0)
        return 0;
    out = mbox_prop_get(MBOX_TAG_GET_BOARD_MODEL);
    return out ? out[0] : 0;
}

u32 mbox_get_board_revision(void)
{
    u32 in = 0;
    u32 *out;

    mbox_prop_begin();
    if (mbox_prop_add(MBOX_TAG_GET_BOARD_REVISION, sizeof(in), &in) < 0)
        return 0;
    if (mbox_prop_send() < 0)
        return 0;
    out = mbox_prop_get(MBOX_TAG_GET_BOARD_REVISION);
    return out ? out[0] : 0;
}

int mbox_get_arm_memory(u32 *base, u32 *size)
{
    u32 in[2] = { 0, 0 };
    u32 *out;

    mbox_prop_begin();
    if (mbox_prop_add(MBOX_TAG_GET_ARM_MEMORY, sizeof(in), in) < 0)
        return -1;
    if (mbox_prop_send() < 0)
        return -1;
    out = mbox_prop_get(MBOX_TAG_GET_ARM_MEMORY);
    if (!out)
        return -1;
    if (base)
        *base = out[0];
    if (size)
        *size = out[1];
    return 0;
}

int mbox_get_vc_memory(u32 *base, u32 *size)
{
    u32 in[2] = { 0, 0 };
    u32 *out;

    mbox_prop_begin();
    if (mbox_prop_add(MBOX_TAG_GET_VC_MEMORY, sizeof(in), in) < 0)
        return -1;
    if (mbox_prop_send() < 0)
        return -1;
    out = mbox_prop_get(MBOX_TAG_GET_VC_MEMORY);
    if (!out)
        return -1;
    if (base)
        *base = out[0];
    if (size)
        *size = out[1];
    return 0;
}
