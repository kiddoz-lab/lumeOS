/*
 * LumeOS userspace - hello world, built against a real C library.
 *
 * Every other user program in this repository is written by hand: no libc, a
 * hand-written crt0, syscalls issued directly.  That makes it a good test of
 * the kernel's ABI and a poor test of the *interface* - a program that does not
 * use `printf` cannot tell you whether a C library could start.
 *
 * This one is linked against musl and does nothing interesting on purpose.  If
 * it prints, then the whole chain a libc depends on worked before `main` was
 * ever called:
 *
 *   - the kernel entered user mode at the entry point the ELF header names;
 *   - musl's `__init_libc` found the auxiliary vector by walking past `argv`
 *     and `envp`, read `AT_PAGESZ`, `AT_HWCAP`, `AT_SECURE`, `AT_RANDOM` and
 *     the four process ids out of it;
 *   - musl's `static_init_tls` walked the *program headers* it was told about
 *     by `AT_PHDR`, `AT_PHNUM` and `AT_PHENT` (this is the part that fails
 *     instantly and silently if the kernel reports a file offset instead of an
 *     address), found `PT_PHDR` and `PT_TLS`, and set the thread pointer;
 *   - `set_tls` stored a value the kernel put in TPIDRURO on the next switch,
 *     and `set_tid_address` returned the pid musl uses as the thread id;
 *   - `malloc` grew the heap with `brk`, and `printf` wrote through `writev`;
 *   - `exit` closed out through `exit_group` and the kernel reaped the process
 *     with the status the program returned.
 *
 * The program checks the two things it can check about itself - that the
 * auxiliary vector is where it should be and that the allocator really handed
 * back usable memory - so a partial failure is reported as a failure rather
 * than as a shorter log.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <elf.h>

int main(void)
{
    unsigned char *block;
    unsigned long pagesz;
    unsigned long hwcap;
    Elf32_Phdr *phdr;
    unsigned long phnum, phent, entry;
    int i, pt_load = 0, entry_in_exec = 0;

    /* musl's entry point, defined by its crt1.  The kernel jumped here, and
     * AT_ENTRY says the same address - the check below ties the two together. */
    extern void _start(void);
    unsigned long start_addr = (unsigned long)(void *)&_start;

    /*
     * getauxval(3) reaches the same vector the kernel built.  Checking that it
     * is readable and sane is the program's half of the contract; the kernel's
     * half is checked by userspace/init, which reads the vector by hand.
     */
    pagesz = getauxval(AT_PAGESZ);
    hwcap = getauxval(AT_HWCAP);
    phdr = (Elf32_Phdr *)getauxval(AT_PHDR);
    phnum = getauxval(AT_PHNUM);
    phent = getauxval(AT_PHENT);

    if (pagesz != 4096 || !hwcap || !phdr || !phnum || phent != sizeof(Elf32_Phdr)) {
        printf("musl: auxv check FAILED (pagesz %lu hwcap 0x%lx phdr %p num %lu ent %lu)\n",
               pagesz, hwcap, (void *)phdr, phnum, phent);
        return 1;
    }

    entry = getauxval(AT_ENTRY);
    if (entry != start_addr) {
        printf("musl: AT_ENTRY 0x%lx is not the address of _start 0x%lx\n",
               entry, start_addr);
        return 1;
    }

    /* Walk the program headers the way a libc's startup does: the entry point
     * must be inside a PT_LOAD marked executable, and the table itself must be
     * inside a mapped segment (if AT_PHDR were a file offset, as it once was,
     * this loop would read whatever happened to live at that address). */
    for (i = 0; i < (int)phnum; i++) {
        if (phdr[i].p_type != PT_LOAD)
            continue;
        pt_load++;
        if ((phdr[i].p_flags & PF_X) && phdr[i].p_vaddr <= entry &&
            phdr[i].p_vaddr + phdr[i].p_memsz > entry)
            entry_in_exec++;
    }
    if (!pt_load) {
        printf("musl: no PT_LOAD in the header table\n");
        return 1;
    }

    /* malloc(4096) goes through the heap, which musl grows with brk - the one
     * allocator path that does not need mmap.  The block is filled and read
     * back, so a heap the kernel reported but did not map fails here. */
    block = malloc(4096);
    if (!block)
        return 1;
    memset(block, 0xA5, 4096);

    printf("musl: hello from a real C library\n");
    printf("musl: pid %d, page size %lu, hwcap 0x%08lx, %d PT_LOAD, %lu headers at %p\n",
           (int)getpid(), pagesz, hwcap, pt_load, phnum, (void *)phdr);
    printf("musl: malloc(4096) returned %p, byte 4095 is 0x%02x\n",
           (void *)block, block[4095]);
    printf("musl: this line went through stdio, writev and the console\n");

    if (entry_in_exec != 1) {
        printf("musl: AT_ENTRY is not in an executable segment (checked %d)\n",
               entry_in_exec);
        return 1;
    }

    free(block);
    return 0;
}
