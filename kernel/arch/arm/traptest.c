/*
 * The C half of the exception round-trip probe: the report the assembly fills
 * in, and the hook the syscall entry calls when the probe's SVC arrives.
 *
 * See kernel/include/lume/traptest.h for what the probe is for and
 * kernel/kernel/selftest.c for the checks that read this report.
 */
#include <lume/string.h>
#include <lume/traptest.h>
#include <lume/types.h>

struct traptest_report traptest_report;

void traptest_reset(void)
{
    memset(&traptest_report, 0, sizeof(traptest_report));
}

void traptest_capture(const struct trapframe *tf)
{
    struct traptest_report *r = &traptest_report;
    u32 i;

    for (i = 0; i < TRAPTEST_REGS; i++)
        r->frame[i] = tf->r[i];

    r->frame_sp = tf->sp;
    r->frame_lr = tf->lr;
    r->frame_pc = tf->pc;
    r->frame_cpsr = tf->cpsr;
    r->calls++;
}
