/*
 * LumeOS file descriptor table.
 *
 * Descriptors are per-process indices into a fixed array of pointers to
 * reference-counted struct file objects.  dup()/dup2()/fork() share the
 * object (and therefore the file offset), which is exactly Linux behaviour.
 */
#include <lume/errno.h>
#include <lume/fd.h>
#include <lume/fs.h>
#include <lume/klog.h>
#include <lume/mem.h>
#include <lume/proc.h>
#include <lume/string.h>
#include <lume/types.h>

#define MAX_OPEN_FILES 256

static struct file file_pool[MAX_OPEN_FILES];

static struct file *file_alloc(void)
{
    for (u32 i = 0; i < MAX_OPEN_FILES; i++) {
        if (file_pool[i].refcount == 0 && file_pool[i].node == NULL &&
            file_pool[i].type == FILE_NODE)
            return &file_pool[i];
    }
    return NULL;
}

void fd_init_process(struct process *p)
{
    for (u32 i = 0; i < LUME_MAX_FDS; i++)
        p->fds[i] = NULL;
    p->used_fds = 0;
}

int fd_inherit(struct process *child, struct process *parent)
{
    for (u32 i = 0; i < LUME_MAX_FDS; i++) {
        child->fds[i] = parent->fds[i];
        if (child->fds[i])
            child->fds[i]->refcount++;
    }
    child->used_fds = parent->used_fds;
    return 0;
}

/** Put a file object into the first free descriptor >= `min_fd`. */
int fd_install(struct process *p, struct file *f, int min_fd)
{
    if (!p)
        p = proc_current();
    if (!p || !f)
        return -EBADF;

    for (int i = (min_fd < 0) ? 0 : min_fd; i < LUME_MAX_FDS; i++) {
        if (!p->fds[i]) {
            p->fds[i] = f;
            p->used_fds++;
            return i;
        }
    }
    return -EMFILE;
}

struct file *fd_get(struct process *p, int fd)
{
    if (!p)
        p = proc_current();
    if (!p || fd < 0 || fd >= LUME_MAX_FDS)
        return NULL;
    return p->fds[fd];
}

int fd_close(struct process *p, int fd)
{
    struct file *f;

    if (!p)
        p = proc_current();
    if (!p || fd < 0 || fd >= LUME_MAX_FDS)
        return -EBADF;

    f = p->fds[fd];
    if (!f)
        return -EBADF;

    p->fds[fd] = NULL;
    p->used_fds--;

    if (f->refcount > 0)
        f->refcount--;
    if (f->refcount == 0) {
        if (f->node && f->node->ops && f->node->ops->close)
            f->node->ops->close(f->node);
        if (f->type == FILE_PIPE)
            /* Pipes are not implemented yet (docs/roadmap.md): the file
             * object is simply released here. */
        memset(f, 0, sizeof(*f));
    }
    return 0;
}

int fd_dup(struct process *p, int oldfd, int min_fd)
{
    struct file *f = fd_get(p, oldfd);

    if (!f)
        return -EBADF;
    f->refcount++;
    int newfd = fd_install(p, f, min_fd);
    if (newfd < 0)
        f->refcount--;
    return newfd;
}

int fd_dup2(struct process *p, int oldfd, int newfd)
{
    struct file *f = fd_get(p, oldfd);

    if (!p)
        p = proc_current();
    if (!f)
        return -EBADF;
    if (newfd < 0 || newfd >= LUME_MAX_FDS)
        return -EBADF;
    if (newfd == oldfd)
        return newfd;

    if (p->fds[newfd])
        fd_close(p, newfd);
    f->refcount++;
    p->fds[newfd] = f;
    p->used_fds++;
    return newfd;
}

/** Open a node as a new descriptor (used by open/openat and by devfs init). */
int fd_open_node(struct process *p, struct fs_node *node, u32 flags)
{
    struct file *f;

    if (!node)
        return -ENOENT;

    f = file_alloc();
    if (!f)
        return -ENFILE;

    memset(f, 0, sizeof(*f));
    f->node = node;
    f->flags = flags;
    f->type = FILE_NODE;
    f->refcount = 1;
    f->offset = (flags & O_APPEND) ? node->size : 0;

    if (node->ops && node->ops->open) {
        int ret = node->ops->open(node, flags);

        if (ret < 0) {
            memset(f, 0, sizeof(*f));
            return ret;
        }
    }

    int fd = fd_install(p, f, 0);
    if (fd < 0)
        memset(f, 0, sizeof(*f));
    return fd;
}

/** Create a descriptor backed by an arbitrary file object (pipes, sockets). */
int fd_install_file(struct process *p, struct file *f, int min_fd)
{
    if (!f)
        return -EBADF;
    f->refcount++;
    int fd = fd_install(p, f, min_fd);

    if (fd < 0)
        f->refcount--;
    return fd;
}

struct file *fd_alloc_file(void)
{
    struct file *f = file_alloc();

    if (f)
        memset(f, 0, sizeof(*f));
    return f;
}

void fd_close_all(struct process *p)
{
    for (u32 i = 0; i < LUME_MAX_FDS; i++)
        if (p->fds[i])
            fd_close(p, (int)i);
}
