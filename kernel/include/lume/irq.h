/*
 * LumeOS interrupt controller interface.
 *
 * The implementation is BCM2835-specific (arch/arm/irq.c) but the interface is
 * deliberately portable: drivers register a handler against an interrupt
 * number and the controller layer dispatches to it.
 */
#ifndef LUME_IRQ_H
#define LUME_IRQ_H

#include <lume/trapframe.h>
#include <lume/types.h>

/** Initialise the controller and mask every source. */
void irq_init(void);

/** Register a handler for an interrupt number.  Returns 0 on success. */
int irq_register(u32 irq, const char *name, void (*handler)(u32 irq, void *arg), void *arg);

void irq_unregister(u32 irq);

/** Globally enable/disable IRQ delivery and the CPSR.I mask. */
void irq_enable(void);
void irq_disable(void);
int  irq_is_enabled(void);

u32 irq_total_count(void);
u32 irq_spurious_count(void);

/** Architecture entry points called from the assembly vectors. */
void do_irq(struct trapframe *tf);
void do_syscall(struct trapframe *tf);
void do_dabt(struct trapframe *tf);
void do_pabt(struct trapframe *tf);
void do_undef(struct trapframe *tf);

#endif /* LUME_IRQ_H */
