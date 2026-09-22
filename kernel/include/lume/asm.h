/*
 * CP15 / ARM1176JZF-S system control helpers.
 *
 * ARMv6 is *not* ARMv7: the DMB/DSB/ISB mnemonics and the ARMv7 CP15
 * encodings for them do not exist as instructions here.  The ARM1176 (ARMv6)
 * way to express them is the CP15 "cache, branch predictor and TLB
 * maintenance" operations:
 *
 *   DSB equivalent: mcr p15, 0, r0, c7, c10, 4
 *   DMB equivalent: mcr p15, 0, r0, c7, c10, 5
 *   ISB equivalent: mcr p15, 0, r0, c7, c5, 4
 *
 * (ARM1176JZF-S Technical Reference Manual, chapter 3.2.16 "c7, Cache,
 * branch predictor and TLB maintenance operations".)
 *
 * Everything here is written as static inline functions or macros so that no
 * call-clobbered state is required.  The code is deliberately conservative:
 * it is valid for a uniprocessor ARMv6 system with write-back caches.
 */
#ifndef LUME_ASM_H
#define LUME_ASM_H

#include <lume/types.h>

/* ------------------------------------------------------------------ */
/* CPSR / mode definitions (ARM ARM A2.3 "Program status registers")   */
/* ------------------------------------------------------------------ */
#define MODE_USR 0x10u
#define MODE_FIQ 0x11u
#define MODE_IRQ 0x12u
#define MODE_SVC 0x13u
#define MODE_ABT 0x17u
#define MODE_UND 0x1Bu
#define MODE_SYS 0x1Fu

#define CPSR_MODE_MASK 0x1Fu
#define CPSR_FIQ_BIT   (1u << 6)
#define CPSR_IRQ_BIT   (1u << 7)
#define CPSR_A_BIT     (1u << 8)

/* ------------------------------------------------------------------ */
/* Barriers and cache/TLB maintenance                                  */
/* ------------------------------------------------------------------ */

/** Data Synchronisation Barrier (ARMv6 encoding). */
static inline void arm_dsb(void)
{
    asm volatile("mcr p15, 0, %0, c7, c10, 4" : : "r"(0) : "memory");
}

/** Data Memory Barrier (ARMv6 encoding). */
static inline void arm_dmb(void)
{
    asm volatile("mcr p15, 0, %0, c7, c10, 5" : : "r"(0) : "memory");
}

/** Instruction Synchronisation Barrier (ARMv6 encoding). */
static inline void arm_isb(void)
{
    asm volatile("mcr p15, 0, %0, c7, c5, 4" : : "r"(0) : "memory");
}

static inline void arm_invalidate_icache(void)
{
    asm volatile("mcr p15, 0, %0, c7, c5, 0" : : "r"(0) : "memory");
}

static inline void arm_invalidate_icache_line(void *va)
{
    asm volatile("mcr p15, 0, %0, c7, c5, 1" : : "r"(va) : "memory");
}

static inline void arm_invalidate_branch_predictor(void)
{
    asm volatile("mcr p15, 0, %0, c7, c5, 6" : : "r"(0) : "memory");
}

static inline void arm_dcache_invalidate_all(void)
{
    asm volatile("mcr p15, 0, %0, c7, c6, 0" : : "r"(0) : "memory");
}

static inline void arm_dcache_clean_all(void)
{
    asm volatile("mcr p15, 0, %0, c7, c10, 2" : : "r"(0) : "memory");
}

static inline void arm_dcache_clean_invalidate_all(void)
{
    asm volatile("mcr p15, 0, %0, c7, c14, 0" : : "r"(0) : "memory");
}

static inline void arm_dcache_clean_mva(void *va)
{
    asm volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(va) : "memory");
}

static inline void arm_dcache_clean_invalidate_mva(void *va)
{
    asm volatile("mcr p15, 0, %0, c7, c14, 1" : : "r"(va) : "memory");
}

/** Clean and invalidate the data cache for a virtual range. */
static inline void arm_dcache_clean_invalidate_range(void *start, u32 len)
{
    u32 addr = (u32)start & ~31u;
    u32 end = ((u32)start + len + 31u) & ~31u;

    for (; addr < end; addr += 32)
        arm_dcache_clean_invalidate_mva((void *)addr);
    arm_dsb();
}

static inline void arm_dcache_clean_range(void *start, u32 len)
{
    u32 addr = (u32)start & ~31u;
    u32 end = ((u32)start + len + 31u) & ~31u;

    for (; addr < end; addr += 32)
        arm_dcache_clean_mva((void *)addr);
    arm_dsb();
}

/** Invalidate the entire TLB (unified; ARMv6 has no per-ASID invalidate). */
static inline void arm_tlb_invalidate_all(void)
{
    asm volatile("mcr p15, 0, %0, c8, c7, 0" : : "r"(0) : "memory");
    arm_dsb();
    arm_isb();
}

/** Invalidate one TLB entry by modified virtual address. */
static inline void arm_tlb_invalidate_mva(void *va)
{
    asm volatile("mcr p15, 0, %0, c8, c7, 1" : : "r"(va) : "memory");
    arm_dsb();
}

/* ------------------------------------------------------------------ */
/* Register access                                                     */
/* ------------------------------------------------------------------ */
static inline u32 arm_read_sctlr(void)
{
    u32 v;
    asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v));
    return v;
}

static inline void arm_write_sctlr(u32 v)
{
    asm volatile("mcr p15, 0, %0, c1, c0, 0" : : "r"(v) : "memory");
}

static inline void arm_write_ttbr0(u32 v)
{
    asm volatile("mcr p15, 0, %0, c2, c0, 0" : : "r"(v) : "memory");
    arm_isb();
}

static inline u32 arm_read_ttbr0(void)
{
    u32 v;
    asm volatile("mrc p15, 0, %0, c2, c0, 0" : "=r"(v));
    return v;
}

static inline void arm_write_dacr(u32 v)
{
    asm volatile("mcr p15, 0, %0, c3, c0, 0" : : "r"(v) : "memory");
}

static inline u32 arm_read_dfsr(void)
{
    u32 v;
    asm volatile("mrc p15, 0, %0, c5, c0, 0" : "=r"(v));
    return v;
}

static inline u32 arm_read_ifsr(void)
{
    u32 v;
    asm volatile("mrc p15, 0, %0, c5, c0, 1" : "=r"(v));
    return v;
}

static inline u32 arm_read_dfar(void)
{
    u32 v;
    asm volatile("mrc p15, 0, %0, c6, c0, 0" : "=r"(v));
    return v;
}

static inline u32 arm_read_ifar(void)
{
    u32 v;
    asm volatile("mrc p15, 0, %0, c6, c0, 2" : "=r"(v));
    return v;
}

/* Main ID register: used at boot to print the CPU identity. */
static inline u32 arm_read_midr(void)
{
    u32 v;
    asm volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(v));
    return v;
}

/* Cache type / size: used for diagnostics and cache maintenance decisions. */
static inline u32 arm_read_ctr(void)
{
    u32 v;
    asm volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(v));
    return v;
}

/* ------------------------------------------------------------------ */
/* Thread/process TLS registers (ARMv6K "thread ID" registers)         */
/* ------------------------------------------------------------------ */
/* ARM1176JZF-S implements the ARMv6K thread ID registers:
 *   c13, c0, 2 = User read/write (FCSE PID on older cores)
 *   c13, c0, 3 = User read-only (TPIDRURO - the Linux ARM EABI TLS pointer)
 *   c13, c0, 4 = Privileged only (TPIDRPRW)
 * Linux's __ARM_NR_set_tls writes c13, c0, 3; LumeOS does the same. */
static inline void arm_write_tpidruro(u32 v)
{
    asm volatile("mcr p15, 0, %0, c13, c0, 3" : : "r"(v) : "memory");
}

static inline u32 arm_read_tpidruro(void)
{
    u32 v;
    asm volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
    return v;
}

static inline void arm_write_tpidrprw(u32 v)
{
    asm volatile("mcr p15, 0, %0, c13, c0, 4" : : "r"(v) : "memory");
}

/* Context ID register: ASID in bits [7:0], PROCID in bits [31:8].
 * LumeOS keeps the ASID masked to 0 until ASID support is enabled (see
 * arch/arm/mmu.c: ASIDs are deliberately not used yet because ARMv6 has no
 * ASID-scoped TLB invalidation, which makes page-table reuse subtle). */
static inline void arm_write_contextidr(u32 v)
{
    asm volatile("mcr p15, 0, %0, c13, c0, 1" : : "r"(v) : "memory");
}

/* ------------------------------------------------------------------ */
/* Interrupt control                                                   */
/* ------------------------------------------------------------------ */
static inline u32 arm_irq_save(void)
{
    u32 flags, tmp = 1u;
    asm volatile("mrs %0, cpsr\n"
                 "\tcpsid if\n"
                 : "=r"(flags), "+r"(tmp));
    return flags & (CPSR_IRQ_BIT | CPSR_FIQ_BIT);
}

static inline void arm_irq_restore(u32 flags)
{
    if (flags & CPSR_IRQ_BIT)
        asm volatile("cpsie i" ::: "memory");
    if (flags & CPSR_FIQ_BIT)
        asm volatile("cpsie f" ::: "memory");
}

static inline void arm_irq_enable(void)
{
    asm volatile("cpsie if" ::: "memory");
}

static inline void arm_irq_disable(void)
{
    asm volatile("cpsid if" ::: "memory");
}

static inline u32 arm_read_cpsr(void)
{
    u32 v;
    asm volatile("mrs %0, cpsr" : "=r"(v));
    return v;
}

/** Wait for an interrupt (ARMv6 has WFI through c7, c0, 4). */
static inline void arm_wfi(void)
{
    asm volatile("mcr p15, 0, %0, c7, c0, 4" : : "r"(0) : "memory");
}

/* Enter user mode with the given entry point, stack pointer and (r0) argument.
 * Never returns to the caller: control resumes in user mode. */
static inline void __attribute__((noreturn))
arm_enter_usermode(u32 entry, u32 sp, u32 arg)
{
    register u32 r0 asm("r0") = arg;
    register u32 r1 asm("r1") = sp;
    register u32 r2 asm("r2") = entry;

    asm volatile("msr cpsr_c, %0\n"
                 "\tmov sp, %1\n"
                 "\tmov pc, %2\n"
                 :
                 : "r"((u32)MODE_USR), "r"(r1), "r"(r2), "r"(r0)
                 : "memory");
    __builtin_unreachable();
}

#endif /* LUME_ASM_H */
