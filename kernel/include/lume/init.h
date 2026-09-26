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

/*
 * The second program: `userspace/musl-hello`, built against musl and linked by
 * zig, embedded the same way.  It exists to answer a question init cannot -
 * *can a real C library start here* - and the answer is only interesting while
 * it is a separate program, because the point is that nothing in this kernel
 * knows or cares that a libc wrote it.
 *
 * It is loaded by the same elf_load(), from the same kind of blob, and runs as
 * a second process with its own address space.  When there is a filesystem it
 * becomes /bin/musl-hello and this declaration disappears.
 */
extern const u8 lume_musl_elf[];
extern const u32 lume_musl_elf_size;
extern const u32 lume_musl_elf_entry;
extern const char lume_musl_elf_sha256[];

/* The stack init runs on is built by kernel/kernel/ustack.c from the bounds in
 * config.h (LUME_USER_STACK_TOP / _MIN_PAGES / _MAX); nothing about it is
 * specific to init, because exec(2) will build one the same way. */

/** Load and start init.  Returns the process, or NULL if it could not be
 *  started (the caller then decides what to do instead). */
struct process *init_start(struct process *launcher);

#endif /* LUME_INIT_H */
