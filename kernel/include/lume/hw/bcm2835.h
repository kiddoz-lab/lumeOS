/*
 * LumeOS hardware definitions for the Broadcom BCM2835 as wired up on the
 * Raspberry Pi Zero W (BCM2835, ARM1176JZF-S, 512 MiB RAM).
 *
 * Every constant in this file is traceable to a public document; the source
 * for each block is cited inline.  See docs/research-notes.md for the full
 * list of references.
 *
 * Primary reference: "BCM2835 ARM Peripherals" (Broadcom, 2012-02-06),
 *   section 1.2 "Address map" and the per-peripheral chapters.  The ARM
 *   (physical) view of the peripherals is documented at 0x20000000 with a
 *   16 MiB window; the VideoCore (bus) view uses 0x7E000000.  Addresses here
 *   are always the ARM physical addresses.
 */
#ifndef LUME_HW_BCM2835_H
#define LUME_HW_BCM2835_H

/* ------------------------------------------------------------------ */
/* Memory map (BCM2835 ARM Peripherals 1.2)                            */
/* ------------------------------------------------------------------ */

/* ARM physical base of RAM. */
#define BCM2835_RAM_BASE        0x00000000u
/* Default RAM size of the Raspberry Pi Zero W (512 MiB total).  The amount
 * actually usable by the ARM core is reported by the firmware (mailbox
 * GET_ARM_MEMORY) or by the device tree; see arch/arm/memdetect.c. */
#define BCM2835_RAM_SIZE_DEFAULT (512u * 1024u * 1024u)

/* ARM physical base of the peripheral window (16 MiB). */
#define BCM2835_PERIPHERAL_BASE 0x20000000u

/* The boot firmware reserves the bottom of RAM for ATAGS/device tree. */
#define BCM2835_FIRMWARE_AREA_END 0x00008000u

/* ------------------------------------------------------------------ */
/* Core peripherals (BCM2835 ARM Peripherals 1.2)                      */
/* ------------------------------------------------------------------ */

/* System timer: section 1.2 and chapter 12.  Offsets: 4.1 CS, 4.2 CLO,
 * 4.3 CHI, 4.4 C0..C3 (the registers are named "System timer registers"
 * but live in chapter 12 of the datasheet). */
#define BCM2835_SYSTIMER_BASE   0x20003000u

/* Interrupt controller: chapter 7.  Register block starts at 0x2000B000
 * but the first documented register (IRQ basic pending) is at +0x200. */
#define BCM2835_IRQ_BASE        0x2000B000u
#define BCM2835_IRQ_REG_OFFSET  0x200u

/* ARM timer: chapter 14 (ARM Timer).  Same 4 KiB as the interrupt
 * controller block. */
#define BCM2835_ARMTIMER_BASE   0x2000B000u

/* Mailboxes: chapter 13 (Mailboxes).  Mailbox 0 for ARM->VC (channel 8 is
 * the property interface), mailbox 1 for VC->ARM. */
#define BCM2835_MBOX_BASE       0x2000B880u

/* Power management / reset: chapter 13.5 (PM registers, "Watchdog"). */
#define BCM2835_PM_BASE         0x20100000u
#define BCM2835_WDOG_OFFSET     0x24u

/* GPIO: chapter 6. */
#define BCM2835_GPIO_BASE       0x20200000u

/* The two UARTs are documented in chapter 13 "UART" of the BCM2835 ARM
 * Peripherals document (PL011 is chapter 13, "Universal Asynchronous
 * Receiver/Transmitter"). */
#define BCM2835_UART0_BASE      0x20201000u  /* PL011 */
#define BCM2835_UART1_BASE      0x20215000u  /* mini UART (SPI0/MU block) */

/* SD host controllers: chapter 5 (EMMC/SD card controller @ 0x20300000)
 * and chapter 6.3? -- SDHOST is documented in "BCM2835 ARM Peripherals"
 * chapter 5 as the "SD/EMMC" controller at 0x20300000; the secondary
 * "SDHOST" register block is at 0x20202000 (see BCM2835 datasheet
 * section "SDHOST" and the Linux binding brcm,bcm2835-sdhost). */
#define BCM2835_EMMC_BASE       0x20300000u
#define BCM2835_SDHOST_BASE     0x20202000u

/* SPI0: chapter 10 (SPI).  Needed for the future 3.5" touch panel. */
#define BCM2835_SPI0_BASE       0x20204000u

/* Clocks (CPRMAN) and power: chapter 13 "Clock manager" / 13.5. */
#define BCM2835_CM_BASE         0x20101000u

/* GPU: the VideoCore firmware ("start.elf") runs before the ARM core and
 * owns the top of RAM for its own use (framebuffer, etc.).  The amount is
 * configured with config.txt "gpu_mem="; the ARM-usable range is queried
 * from the firmware. */

/* ------------------------------------------------------------------ */
/* GPIO / pin function select (BCM2835 ARM Peripherals 6.1, 6.2)       */
/* ------------------------------------------------------------------ */
#define GPIO_FSEL0   0x00u  /* GPIO 0-9   function select */
#define GPIO_FSEL1   0x04u  /* GPIO 10-19 */
#define GPIO_FSEL2   0x08u  /* GPIO 20-29 */
#define GPIO_FSEL3   0x0Cu  /* GPIO 30-39 */
#define GPIO_FSEL4   0x10u  /* GPIO 40-49 */
#define GPIO_FSEL5   0x14u  /* GPIO 50-53 */
#define GPIO_SET0    0x1Cu
#define GPIO_SET1    0x20u
#define GPIO_CLR0    0x28u
#define GPIO_CLR1    0x2Cu
#define GPIO_LEV0    0x34u
#define GPIO_LEV1    0x38u
#define GPIO_EDS0    0x40u
#define GPIO_EDS1    0x44u
#define GPIO_REN0    0x4Cu
#define GPIO_REN1    0x50u
#define GPIO_FEN0    0x58u
#define GPIO_FEN1    0x5Cu
#define GPIO_PUD     0x94u  /* pull-up/down control (6.2) */
#define GPIO_PUDCLK0 0x98u
#define GPIO_PUDCLK1 0x9Cu

/* Pin function select values (BCM2835 ARM Peripherals table 6-1). */
#define GPIO_FSEL_INPUT  0u
#define GPIO_FSEL_OUTPUT 1u
#define GPIO_FSEL_ALT0   4u
#define GPIO_FSEL_ALT1   5u
#define GPIO_FSEL_ALT2   6u
#define GPIO_FSEL_ALT3   7u
#define GPIO_FSEL_ALT4   3u
#define GPIO_FSEL_ALT5   2u

/* Pi Zero W 40-pin header (Raspberry Pi docs, "GPIO pinout"): UART0 TXD is
 * GPIO14 (ALT0), RXD is GPIO15 (ALT0). */
#define GPIO_PIN_UART_TXD 14u
#define GPIO_PIN_UART_RXD 15u

/* ------------------------------------------------------------------ */
/* PL011 UART register offsets (BCM2835 ARM Peripherals 13.4)          */
/* ------------------------------------------------------------------ */
#define PL011_DR      0x000u  /* data register */
#define PL011_RSRECR  0x004u
#define PL011_FR      0x018u  /* flag register */
#define PL011_ILPR    0x020u
#define PL011_IBRD    0x024u  /* integer baud divisor */
#define PL011_FBRD    0x028u  /* fractional baud divisor */
#define PL011_LCRH    0x02Cu  /* line control */
#define PL011_CR      0x030u  /* control */
#define PL011_IFLS    0x034u
#define PL011_IMSC    0x038u  /* interrupt mask set/clear */
#define PL011_RIS     0x03Cu  /* raw interrupt status */
#define PL011_MIS     0x040u  /* masked interrupt status */
#define PL011_ICR     0x044u  /* interrupt clear */
#define PL011_DMACR   0x048u
#define PL011_ITCR    0x080u
#define PL011_ITIP    0x084u
#define PL011_ITOP    0x088u
#define PL011_TDR     0x08Cu

#define PL011_FR_TXFF (1u << 5)  /* transmit FIFO full */
#define PL011_FR_RXFE (1u << 4)  /* receive FIFO empty */
#define PL011_FR_BUSY (1u << 3)

#define PL011_LCRH_FEN  (1u << 4)  /* FIFO enable */
#define PL011_LCRH_WLEN_8 (3u << 5) /* 8 bits */

#define PL011_CR_UARTEN (1u << 0)
#define PL011_CR_TXE    (1u << 8)
#define PL011_CR_RXE    (1u << 9)

#define PL011_IMSC_RXIM (1u << 4)  /* receive interrupt mask */
#define PL011_IMSC_RTIM (1u << 6)

#define PL011_ICR_RXIC  (1u << 4)
#define PL011_ICR_RTIC  (1u << 6)

/* ------------------------------------------------------------------ */
/* System timer (BCM2835 ARM Peripherals 12.x)                         */
/* ------------------------------------------------------------------ */
#define SYSTIMER_CS   0x00u  /* control/status */
#define SYSTIMER_CLO  0x04u  /* counter lower 32 bits, 1 MHz */
#define SYSTIMER_CHI  0x08u  /* counter upper 32 bits */
#define SYSTIMER_C0   0x0Cu
#define SYSTIMER_C1   0x10u
#define SYSTIMER_C2   0x14u
#define SYSTIMER_C3   0x18u
/* The system timer runs at 1 MHz (BCM2835 ARM Peripherals 12.1:
 * "The system timer is a 64-bit counter which runs at 1MHz"). */
#define SYSTIMER_FREQ_HZ 1000000u
/* Channel 3 is the channel intended for ARM use; channels 0 and 2 are
 * reserved for the VideoCore firmware (see BCM2835 ARM Peripherals
 * "System Timer" and the Embedded Xinu/Linux usage notes).  Channel 1 is
 * also free.  IRQ numbers: shared IRQ 1 = C1, IRQ 3 = C3. */
#define SYSTIMER_IRQ_CHANNEL 3u

/* ------------------------------------------------------------------ */
/* Interrupt controller (BCM2835 ARM Peripherals 7.5)                  */
/* ------------------------------------------------------------------ */
#define IRQ_BASIC_PENDING 0x200u  /* 0x2000B200 */
#define IRQ_PENDING_1     0x204u  /* shared IRQs 0-31 */
#define IRQ_PENDING_2     0x208u  /* shared IRQs 32-63 */
#define IRQ_FIQ_CONTROL   0x20Cu
#define IRQ_ENABLE_1      0x210u
#define IRQ_ENABLE_2      0x214u
#define IRQ_ENABLE_BASIC  0x218u
#define IRQ_DISABLE_1     0x21Cu
#define IRQ_DISABLE_2     0x220u
#define IRQ_DISABLE_BASIC 0x224u

/* Shared IRQ numbers (BCM2835 ARM Peripherals 7.5, table 7-4, plus the
 * well known omissions documented by Embedded Xinu and Linux's
 * irq-bcm2835.c: 0-3 are the four system timer comparators, 9 is the USB
 * controller). */
#define IRQ_SYSTIMER_C0 0u
#define IRQ_SYSTIMER_C1 1u
#define IRQ_SYSTIMER_C2 2u
#define IRQ_SYSTIMER_C3 3u
#define IRQ_USB         9u
#define IRQ_PL011       57u   /* UART0 */
#define IRQ_EMMC        62u   /* SD card controller */
#define IRQ_I2C_SPI_I2S 43u

/* ARM-local (basic) IRQ numbers: IRQ_BASIC_PENDING bits 0-7 are the shared
 * IRQ groups; bits 8-11 are the ARM-local interrupts listed in
 * BCM2835 ARM Peripherals table 7-5. */
#define IRQ_BASIC_TIMER   (1u << 8)   /* ARM timer */
#define IRQ_BASIC_MAILBOX (1u << 9)
#define IRQ_BASIC_DOORBELL0 (1u << 10)
#define IRQ_BASIC_DOORBELL1 (1u << 11)
#define IRQ_BASIC_GPU0HALTED (1u << 12)
#define IRQ_BASIC_GPU1HALTED (1u << 13)

/* ------------------------------------------------------------------ */
/* Mailbox (BCM2835 ARM Peripherals 13.x "Mailboxes")                  */
/* ------------------------------------------------------------------ */
#define MBOX_READ    0x00u  /* mailbox 0 read (VC -> ARM) */
#define MBOX_PEEK    0x10u
#define MBOX_SENDER  0x14u
#define MBOX_STATUS  0x18u
#define MBOX_CONFIG  0x1Cu
#define MBOX_WRITE   0x20u  /* mailbox 0 write (ARM -> VC) */

#define MBOX_STATUS_FULL  (1u << 31)
#define MBOX_STATUS_EMPTY (1u << 30)

/* Channel numbers (BCM2835 ARM Peripherals 13.1 and the Raspberry Pi
 * firmware wiki "Mailbox property interface"): channel 8 is the property
 * tag interface used by this driver. */
#define MBOX_CH_POWER    0u
#define MBOX_CH_FB       1u
#define MBOX_CH_VCHIQ    3u
#define MBOX_CH_PROPERTY 8u

/* ------------------------------------------------------------------ */
/* Watchdog / PM (BCM2835 ARM Peripherals 13.5 "Power Management")     */
/* ------------------------------------------------------------------ */
#define PM_WDOG        0x24u
#define PM_RSTC        0x1Cu
#define PM_RSTS        0x20u
#define PM_PASSWORD    0x5A000000u
#define PM_RSTC_WRCFG_FULL_RESET 0x00000020u

/* ------------------------------------------------------------------ */
/* EMMC / SD card controller (BCM2835 ARM Peripherals chapter 5)       */
/* ------------------------------------------------------------------ */
#define EMMC_ARG2        0x00u
#define EMMC_BLKSIZECNT  0x04u
#define EMMC_ARG1        0x08u
#define EMMC_CMDTM       0x0Cu
#define EMMC_RESP0       0x10u
#define EMMC_RESP1       0x14u
#define EMMC_RESP2       0x18u
#define EMMC_RESP3       0x1Cu
#define EMMC_DATA        0x20u
#define EMMC_STATUS      0x24u
#define EMMC_CONTROL0    0x28u
#define EMMC_CONTROL1    0x2Cu
#define EMMC_INTERRUPT   0x30u
#define EMMC_IRPT_MASK   0x34u
#define EMMC_IRPT_EN     0x38u
#define EMMC_CONTROL2    0x3Cu
#define EMMC_SLOTISR_VER 0xFCu

/* CMDTM flags */
#define EMMC_CMDTM_CMD_INDEX(x)   ((x) & 0x3Fu)
#define EMMC_CMDTM_CMDTYPE_BC     (1u << 6)
#define EMMC_CMDTM_CMDTYPE_BCR    (2u << 6)
#define EMMC_CMDTM_CMDTYPE_AC     (3u << 6)
#define EMMC_CMDTM_CMDTYPE_ADTC   (4u << 6)
#define EMMC_CMDTM_RSPNS_48       (1u << 16)
#define EMMC_CMDTM_RSPNS_136      (1u << 17)
#define EMMC_CMDTM_RSPNS_48B      (3u << 16)
#define EMMC_CMDTM_CMD_ISDATA     (1u << 21)
#define EMMC_CMDTM_CMD_IXCHK_EN   (1u << 22)
#define EMMC_CMDTM_CMD_CRCCHK_EN  (1u << 23)

#define EMMC_STATUS_CMD_INHIBIT   (1u << 0)
#define EMMC_STATUS_DAT_INHIBIT   (1u << 1)
#define EMMC_STATUS_READ_TRANSFER  (1u << 9)
#define EMMC_STATUS_WRITE_TRANSFER (1u << 10)
#define EMMC_STATUS_DAT_ACTIVE    (1u << 2)

#define EMMC_CONTROL0_HCTL_DWIDTH (1u << 1)  /* 4-bit data width */
#define EMMC_CONTROL0_HCTL_HS_EN  (1u << 2)
#define EMMC_CONTROL0_HCTL_GPIOLVL (1u << 3)

#define EMMC_CONTROL1_CLK_INTLEN  (1u << 0)  /* internal clock enable */
#define EMMC_CONTROL1_CLK_STABLE  (1u << 1)
#define EMMC_CONTROL1_CLK_EN      (1u << 2)
#define EMMC_CONTROL1_CLK_MASK    0xFFF00000u
/* The EMMC clock is derived from the "core clock"; SD card identification
 * must happen at <= 400 kHz, data transfers can run at 25 MHz.  The base
 * (SD) clock of the BCM2835 EMMC block is 250 MHz (BCM2835 ARM Peripherals
 * 5.2, table 5-3: "SD clock frequency = base clock / divisor", base clock
 * from CM_EMMCCTL).  We query the actual clock from the firmware, exactly
 * like the Linux driver does (see arch/arm/drivers/sd/emmc.c). */

#define EMMC_INTERRUPT_CMD_DONE   (1u << 0)
#define EMMC_INTERRUPT_DATA_DONE  (1u << 1)
#define EMMC_INTERRUPT_WRITE_RDY  (1u << 4)
#define EMMC_INTERRUPT_READ_RDY   (1u << 5)
#define EMMC_INTERRUPT_ERR_MASK   0xFFFF0000u  /* documented error bits */

/* Firmware clock IDs for the mailbox GET_CLOCK_RATE / SET_CLOCK_RATE tags.
 * These match the device-tree clock bindings (include/dt-bindings/clock/
 * bcm2835.h upstream) and the firmware's property interface. */
#define BCM2835_CLK_EMMC   1u
#define BCM2835_CLK_UART   2u
#define BCM2835_CLK_ARM    3u
#define BCM2835_CLK_CORE   4u
#define BCM2835_CLK_V3D    5u
#define BCM2835_CLK_SDRAM  8u
#define BCM2835_CLK_PIXEL  9u

#endif /* LUME_HW_BCM2835_H */
