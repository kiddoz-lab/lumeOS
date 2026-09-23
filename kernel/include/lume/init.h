/*
 * The first user program.
 *
 * There is no filesystem yet, so init arrives embedded in the kernel image
 * (tools/embed_user.py turns the static ELF that `make userspace` builds into
 * a byte array).  The loader that runs it is the same elf_load() a
 * filesystem-backed /bin/init will use - what is missing is the path lookup,
 * not the loading.
 *
 * The symbols below are generated; the sizes and the digest are what make it
 * possible to say in a log line *which* init a kernel booted.
 */
#ifndef LUME_INIT_H
#define LUME_INIT_H

#include <lume/types.h>

extern const u8 lume_init_elf[];
extern const u32 lume_init_elf_size;
extern const u32 lume_init_elf_entry;
extern const char lume_init_elf_sha256[];

/* Where the user stack is mapped from: the top of the stack region, one page
 * down per page needed. */
#define LUME_INIT_STACK_PAGES 2

/** Load and start init.  Returns the process, or NULL if it could not be
 *  started (the caller then decides what to do instead). */
struct process *init_start(struct process *launcher);

#endif /* LUME_INIT_H */
