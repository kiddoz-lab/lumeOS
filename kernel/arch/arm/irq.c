/*
 * LumeOS BCM2835 interrupt controller driver and dispatch.
 *
 * Register offsets and bit assignments: BCM2835 ARM Peripherals chapter 7.
 * The controller is enabled only after the vector table, the per-mode stacks
 * and the peripheral mappings are all in place, so an interrupt can never
 * arrive before the kernel is able to handle it.
 *
 * Handlers run with interrupts still masked (the trap entry leaves CPSR.I
 * set); a handler that needs to wait does so through the scheduler, which
 * re-enables interrupts in its idle path.  This keeps the handler code
 * non-reentrant and trivially safe on a uniprocessor.
 */
#include <lume/asm.h>
#include <lume/hw/bcm2835.h>
#include <lume/irq.h>
#include <lume/klog.h>
#include <lume/panic.h>
#include <lume/sched.h>
#include <lume/uart.h>
#include <lume/time.h>
#include <lume/string.h>
#include <lume/time.h>
#include <lume/trapframe.h>
#include <lume/types.h>

#define IRQ_REG(off) (*(volatile u32 *)((u32)PERIPHERAL_TO_VIRT(BCM2835_IRQ_BASE) + (off)))

static struct irq_chip_data {
    const char *name;
    void (*handler)(u32 irq, void *arg);
    void *arg;
    u32 irq;
    int used;
    /* Some sources cannot be told apart from the shared pending registers: the
     * SoC aggregates a few device lines into the basic pending register, where
     * the source is no longer identifiable.  A driver for such a source checks
     * its own status register inside the handler and does nothing when the
     * source is not actually asserting (the system timer works exactly that
     * way).  Flagging those handlers lets do_irq() give them a chance even when
     * pending_1/pending_2 are empty - without it, a level-asserted line is
     * never acknowledged and the CPU takes the same interrupt forever. */
    int self_checking;
} irq_table[32];

static u32 irq_table_used;
static u32 irq_spurious;
static u32 irq_basic_unhandled;
static u32 irq_unclaimed;      /* IRQs where no shared source was pending */
static u32 irq_fallback;       /* of those, ones a self-checking driver serviced */
static u32 irq_storm_reports;
static u32 irq_count;
static int irq_enabled;

void irq_init(void)
{
    memset(irq_table, 0, sizeof(irq_table));
    irq_table_used = 0;
    irq_spurious = 0;
    irq_unclaimed = 0;
    irq_fallback = 0;
    irq_storm_reports = 0;
    irq_count = 0;
    irq_enabled = 0;

    /* Mask everything, then clear any latched state. */
    IRQ_REG(IRQ_DISABLE_1) = 0xFFFFFFFFu;
    IRQ_REG(IRQ_DISABLE_2) = 0xFFFFFFFFu;
    IRQ_REG(IRQ_DISABLE_BASIC) = 0xFFFFFFFFu;
    IRQ_REG(IRQ_BASIC_PENDING) = 0; /* pending bits are cleared at the source */
}

int irq_register(u32 irq, const char *name, void (*handler)(u32, void *), void *arg)
{
    return irq_register_flags(irq, name, handler, arg, 0);
}

int irq_register_flags(u32 irq, const char *name, void (*handler)(u32, void *),
                       void *arg, int self_checking)
{
    if (irq_table_used >= ARRAY_SIZE(irq_table))
        return -1;

    irq_table[irq_table_used].name = name;
    irq_table[irq_table_used].handler = handler;
    irq_table[irq_table_used].arg = arg;
    irq_table[irq_table_used].irq = irq;
    irq_table[irq_table_used].used = 1;
    irq_table[irq_table_used].self_checking = self_checking;
    irq_table_used++;

    if (irq < 32)
        IRQ_REG(IRQ_ENABLE_1) = 1u << irq;
    else if (irq < 64)
        IRQ_REG(IRQ_ENABLE_2) = 1u << (irq - 32);
    else
        return -1;

    pr_debug("irq: registered %s on IRQ %u", name, irq);
    return 0;
}

void irq_unregister(u32 irq)
{
    if (irq < 32)
        IRQ_REG(IRQ_DISABLE_1) = 1u << irq;
    else if (irq < 64)
        IRQ_REG(IRQ_DISABLE_2) = 1u << (irq - 32);

    for (u32 i = 0; i < irq_table_used; i++) {
        if (irq_table[i].used && irq_table[i].irq == irq) {
            irq_table[i].used = 0;
            return;
        }
    }
}

void irq_enable(void)
{
    irq_enabled = 1;
    arm_irq_enable();
}

void irq_disable(void)
{
    arm_irq_disable();
    irq_enabled = 0;
}

int irq_is_enabled(void)
{
    return irq_enabled;
}

static int irq_handle_one(u32 irq)
{
    for (u32 i = 0; i < irq_table_used; i++) {
        if (irq_table[i].used && irq_table[i].irq == irq) {
            irq_table[i].handler(irq, irq_table[i].arg);
            return 1;
        }
    }
    irq_spurious++;
    pr_debug("irq: no handler for IRQ %u", irq);
    return 0;
}

/* Give every self-checking handler a chance to look at its own device.  Returns
 * how many of them did something. */
static u32 irq_service_self_checking(void)
{
    u32 serviced = 0;

    for (u32 i = 0; i < irq_table_used; i++) {
        if (irq_table[i].used && irq_table[i].self_checking) {
            irq_table[i].handler(irq_table[i].irq, irq_table[i].arg);
            serviced++;
        }
    }
    return serviced;
}

/*
 * The interrupt arrived but no shared source is pending.  Print the state of
 * everything that could be responsible - once, because a level-triggered line
 * that is never acknowledged produces this situation thousands of times per
 * second, and a per-interrupt print would flood the console instead of
 * explaining it.
 */
static void irq_report_unclaimed(u32 pending1, u32 pending2, u32 basic)
{
    if (irq_unclaimed == 1 || (irq_unclaimed % 100000u) == 0) {
        if (irq_storm_reports < 4) {
            irq_storm_reports++;
            pr_warn("irq: %u interrupts with nothing pending in pending_1/2 "
                    "(basic 0x%08x); uart mis 0x%08x, timer cs 0x%08x",
                    irq_unclaimed, basic, uart_irq_status(), timer_status());
        }
    }
    (void)pending1;
    (void)pending2;
}

/**
 * Called from the assembly vector with a trap frame describing the interrupted
 * context.  Returns with interrupts masked; __restore_regs() puts the context
 * back (the scheduler may have changed it in the meantime).
 */
void do_irq(struct trapframe *tf)
{
    u32 pending1 = IRQ_REG(IRQ_PENDING_1);
    u32 pending2 = IRQ_REG(IRQ_PENDING_2);
    u32 basic = IRQ_REG(IRQ_BASIC_PENDING);

    irq_count++;

    /* IRQ pending 1/2 cover the 64 shared interrupt sources (system timer
     * comparators, USB, SD, PL011, ...).  The first eight of them are also
     * mirrored into the low bits of the basic pending register; decoding them
     * from the shared registers alone avoids handling one interrupt twice. */
    u32 claimed = 0;

    for (u32 i = 0; i < 32; i++)
        if (pending1 & (1u << i))
            claimed += irq_handle_one(i);
    for (u32 i = 0; i < 32; i++)
        if (pending2 & (1u << i))
            claimed += irq_handle_one(32 + i);

    if (!claimed) {
        /* Nothing identifiable in the shared registers.  Give the drivers whose
         * device is aggregated into the basic pending register (see the
         * self_checking comment above) the chance to notice their own
         * interrupt, then report the situation if even they saw nothing. */
        irq_unclaimed++;
        irq_fallback += irq_service_self_checking();
        irq_report_unclaimed(pending1, pending2, basic);
    }

    /* ARM-local sources that exist only in the basic pending register. */
    if (basic & (IRQ_BASIC_TIMER | IRQ_BASIC_MAILBOX | IRQ_BASIC_DOORBELL0 |
                 IRQ_BASIC_DOORBELL1 | IRQ_BASIC_GPU0HALTED | IRQ_BASIC_GPU1HALTED))
        irq_basic_unhandled++;

    /* The timer handler calls sched_tick(); here we only act on the result:
     * a trap taken from user mode is the one place where LumeOS preempts. */
    if (sched_need_resched() && tf && (tf->cpsr & 0x1F) == MODE_USR)
        schedule();
}

u32 irq_unclaimed_count(void)
{
    return irq_unclaimed;
}

u32 irq_fallback_count(void)
{
    return irq_fallback;
}

u32 irq_spurious_count(void)
{
    return irq_spurious;
}

u32 irq_total_count(void)
{
    return irq_count;
}
