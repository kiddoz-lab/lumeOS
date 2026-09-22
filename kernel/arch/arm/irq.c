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
#include <lume/klog.h>
#include <lume/panic.h>
#include <lume/sched.h>
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
} irq_table[32];

static u32 irq_table_used;
static u32 irq_spurious;
static u32 irq_basic_unhandled;
static u32 irq_count;
static int irq_enabled;

void irq_init(void)
{
    memset(irq_table, 0, sizeof(irq_table));
    irq_table_used = 0;
    irq_spurious = 0;
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
    if (irq_table_used >= ARRAY_SIZE(irq_table))
        return -1;

    irq_table[irq_table_used].name = name;
    irq_table[irq_table_used].handler = handler;
    irq_table[irq_table_used].arg = arg;
    irq_table[irq_table_used].irq = irq;
    irq_table[irq_table_used].used = 1;
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

static void irq_handle_one(u32 irq)
{
    for (u32 i = 0; i < irq_table_used; i++) {
        if (irq_table[i].used && irq_table[i].irq == irq) {
            irq_table[i].handler(irq, irq_table[i].arg);
            return;
        }
    }
    irq_spurious++;
    pr_debug("irq: no handler for IRQ %u", irq);
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
    for (u32 i = 0; i < 32; i++)
        if (pending1 & (1u << i))
            irq_handle_one(i);
    for (u32 i = 0; i < 32; i++)
        if (pending2 & (1u << i))
            irq_handle_one(32 + i);

    /* ARM-local sources that exist only in the basic pending register. */
    if (basic & (IRQ_BASIC_TIMER | IRQ_BASIC_MAILBOX | IRQ_BASIC_DOORBELL0 |
                 IRQ_BASIC_DOORBELL1 | IRQ_BASIC_GPU0HALTED | IRQ_BASIC_GPU1HALTED))
        irq_basic_unhandled++;

    /* The timer handler calls sched_tick(); here we only act on the result:
     * a trap taken from user mode is the one place where LumeOS preempts. */
    if (sched_need_resched() && tf && (tf->cpsr & 0x1F) == MODE_USR)
        schedule();
}

u32 irq_spurious_count(void)
{
    return irq_spurious;
}

u32 irq_total_count(void)
{
    return irq_count;
}
