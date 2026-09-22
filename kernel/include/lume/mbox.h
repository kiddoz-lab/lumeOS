/*
 * LumeOS Raspberry Pi firmware mailbox driver.
 *
 * The ARM core talks to the VideoCore firmware over mailbox 0 using the
 * "property interface" (channel 8).  The message format is documented in the
 * Raspberry Pi firmware wiki page "Mailbox property interface":
 *
 *   u32 size        - total buffer size in bytes, including this word
 *   u32 status      - request code (0) / response code (0x80000000 = success)
 *   tag list        - each tag is {u32 id, u32 value_size, u32 value_len, data}
 *   u32 end tag     - 0
 *
 * The buffer must be 16-byte aligned because only the upper 28 bits of its
 * address fit in the mailbox register alongside the 4-bit channel number.
 */
#ifndef LUME_MBOX_H
#define LUME_MBOX_H

#include <lume/types.h>

#define MBOX_STATUS_SUCCESS 0x80000000u
#define MBOX_STATUS_ERROR   0x80000001u

/* Property tag identifiers used by LumeOS (firmware wiki). */
#define MBOX_TAG_GET_FIRMWARE_REVISION 0x00000001u
#define MBOX_TAG_GET_BOARD_MODEL       0x00010001u
#define MBOX_TAG_GET_BOARD_REVISION    0x00010002u
#define MBOX_TAG_GET_BOARD_MAC         0x00010003u
#define MBOX_TAG_GET_BOARD_SERIAL      0x00010004u
#define MBOX_TAG_GET_ARM_MEMORY        0x00010005u
#define MBOX_TAG_GET_VC_MEMORY         0x00010006u
#define MBOX_TAG_GET_CLOCKS            0x00010007u
#define MBOX_TAG_GET_CMDLINE           0x00050001u
#define MBOX_TAG_GET_CLOCK_STATE       0x00030001u
#define MBOX_TAG_GET_CLOCK_RATE        0x00030002u
#define MBOX_TAG_SET_CLOCK_RATE        0x00038002u
#define MBOX_TAG_GET_MAX_CLOCK_RATE    0x00030004u
#define MBOX_TAG_GET_MIN_CLOCK_RATE    0x00030007u
#define MBOX_TAG_GET_VOLTAGE           0x00030003u
#define MBOX_TAG_GET_TEMPERATURE       0x00030006u

/* Framebuffer tags (firmware wiki "Mailbox property interface", Frame Buffer). */
#define MBOX_TAG_FB_ALLOCATE           0x00040001u
#define MBOX_TAG_FB_RELEASE            0x00048001u
#define MBOX_TAG_FB_GET_PHYS_WH        0x00040003u
#define MBOX_TAG_FB_SET_PHYS_WH        0x00048003u
#define MBOX_TAG_FB_GET_VIRT_WH        0x00040004u
#define MBOX_TAG_FB_SET_VIRT_WH        0x00048004u
#define MBOX_TAG_FB_GET_DEPTH          0x00040005u
#define MBOX_TAG_FB_SET_DEPTH          0x00048005u
#define MBOX_TAG_FB_GET_PIXEL_ORDER    0x00040006u
#define MBOX_TAG_FB_SET_PIXEL_ORDER    0x00048006u
#define MBOX_TAG_FB_GET_ALPHA_MODE     0x00040007u
#define MBOX_TAG_FB_SET_ALPHA_MODE     0x00048007u
#define MBOX_TAG_FB_GET_PITCH          0x00040008u
#define MBOX_TAG_FB_GET_VIRT_OFFSET    0x00040009u
#define MBOX_TAG_FB_SET_VIRT_OFFSET    0x00048009u
#define MBOX_TAG_FB_GET_OVERSCAN       0x0004000Au
#define MBOX_TAG_FB_SET_OVERSCAN       0x0004800Au

void mbox_init(void);

/** Send a property tag message.  The caller builds the message; this function
 *  performs the mailbox handshake.  Returns 0 on success. */
int mbox_call(u32 *msg, u32 channel);

/** Convenience: property call using an internal static buffer.  The buffer is
 *  returned so the caller can inspect the response.  Returns 0 on success. */
int mbox_property(u32 *tags, u32 tag_words);

u32 mbox_get_firmware_revision(void);
u32 mbox_get_clock_rate(u32 clock_id);
u32 mbox_get_max_clock_rate(u32 clock_id);
int mbox_get_arm_memory(u32 *base, u32 *size);
int mbox_get_vc_memory(u32 *base, u32 *size);
u32 mbox_get_board_model(void);
u32 mbox_get_board_revision(void);

#endif /* LUME_MBOX_H */
