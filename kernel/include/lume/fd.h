/*
 * LumeOS file descriptor table (kernel interface).
 *
 * The implementation is kernel/kernel/fd.c; this header exists because the
 * syscall layer, the process layer and the shell all need the same operations
 * and having each of them declare `extern` prototypes by hand is how the three
 * drift apart.
 */
#ifndef LUME_FD_H
#define LUME_FD_H

#include <lume/fs.h>
#include <lume/proc.h>
#include <lume/types.h>

void fd_init_process(struct process *p);

/** fork(2) semantics: share the parent's open files (refcount++). */
int fd_inherit(struct process *child, struct process *parent);

/** Put a file object in the first free descriptor >= min_fd. */
int fd_install(struct process *p, struct file *f, int min_fd);

/** The file behind a descriptor, or NULL (p == NULL means "current process"). */
struct file *fd_get(struct process *p, int fd);

int fd_close(struct process *p, int fd);
void fd_close_all(struct process *p);
int fd_dup(struct process *p, int oldfd, int min_fd);
int fd_dup2(struct process *p, int oldfd, int newfd);

/** Open a filesystem node and return the new descriptor. */
int fd_open_node(struct process *p, struct fs_node *node, u32 flags);

/** Install an already-built file object (used by console setup and pipes). */
int fd_install_file(struct process *p, struct file *f, int min_fd);

/** Allocate an empty file object from the pool. */
struct file *fd_alloc_file(void);

#endif /* LUME_FD_H */
