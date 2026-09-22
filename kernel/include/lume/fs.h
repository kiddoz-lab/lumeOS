/*
 * LumeOS virtual filesystem.
 *
 * A small node-based VFS in the spirit of "everything is a file": device
 * nodes, RAM files, FAT32 files and /proc entries all implement the same
 * operations table.  Paths are resolved component by component from the
 * process's current directory (or the root when absolute).
 *
 * Deliberately absent: page cache, mount namespaces, hard links, VFS locking.
 * This is a 512 MiB single-core target; the VFS is sized accordingly.
 */
#ifndef LUME_FS_H
#define LUME_FS_H

#include <lume/fcntl.h>
#include <lume/types.h>

#define LUME_NAME_MAX 128
#define LUME_PATH_MAX 256

#define FS_TYPE_NONE   0
#define FS_TYPE_FILE   1
#define FS_TYPE_DIR    2
#define FS_TYPE_CHAR   3   /* character device */
#define FS_TYPE_BLOCK  4   /* block device */
#define FS_TYPE_SYMLINK 5

/* Linux-compatible file type bits used by stat64/st_mode. */
#define S_IFMT  0170000
#define S_IFSOCK 0140000
#define S_IFLNK 0120000
#define S_IFREG 0100000
#define S_IFBLK 0060000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFIFO 0010000
#define S_ISUID 0004000
#define S_ISGID 0002000
#define S_ISVTX 0001000
#define S_IRWXU 0000700
#define S_IRUSR 0000400
#define S_IWUSR 0000200
#define S_IXUSR 0000100
#define S_IRWXG 0000070
#define S_IRGRP 0000040
#define S_IWGRP 0000020
#define S_IXGRP 0000010
#define S_IRWXO 0000007
#define S_IROTH 0000004
#define S_IWOTH 0000002
#define S_IXOTH 0000001

struct fs_node;

struct fs_ops {
    int  (*read)(struct fs_node *node, u32 offset, void *buf, u32 len);
    int  (*write)(struct fs_node *node, u32 offset, const void *buf, u32 len);
    int  (*readdir)(struct fs_node *node, u32 index, char *name, u32 name_len,
                    u32 *type_out, u32 *size_out);
    struct fs_node *(*lookup)(struct fs_node *dir, const char *name);
    int  (*create)(struct fs_node *dir, const char *name, u32 type, u32 mode);
    int  (*unlink)(struct fs_node *dir, const char *name);
    int  (*mkdir)(struct fs_node *dir, const char *name, u32 mode);
    int  (*truncate)(struct fs_node *node, u32 size);
    /* Character devices: open/close hooks and ioctl. */
    int  (*open)(struct fs_node *node, u32 flags);
    int  (*close)(struct fs_node *node);
    int  (*ioctl)(struct fs_node *node, u32 request, u32 arg);
};

struct fs_node {
    char name[LUME_NAME_MAX];
    u32 type;        /* FS_TYPE_* */
    u32 mode;        /* permission bits (S_IRUSR ...) */
    u32 size;
    u32 inode;
    u32 dev;         /* device number for char/block nodes */
    struct fs_ops *ops;
    void *private;
    u32 flags;       /* filesystem-specific flags (e.g. FS_FLAG_READONLY) */
    struct fs_mount *mount; /* set on the root node of a mounted filesystem */
    u32 nlink;
    u32 uid, gid;
    u64 atime, mtime, ctime; /* nanoseconds since the epoch */
};

#define FS_FLAG_READONLY (1u << 0)

/* An open file object.  Several descriptors may share one (dup/fork). */
struct file {
    struct fs_node *node;   /* NULL for sockets and pipes */
    u32 offset;
    u32 flags;              /* open(2) flags */
    int refcount;
    void *private;          /* pipe end, socket, ... */
    u32 type;               /* FILE_* below */
};

#define FILE_NODE  0
#define FILE_PIPE  1
#define FILE_SOCK  2

struct fs_mount {
    char path[LUME_PATH_MAX];
    struct fs_node *root;
    struct fs_mount *next;
};

void vfs_init(void);
int  vfs_mount(const char *path, struct fs_node *root);
struct fs_node *vfs_root(void);

/** Resolve a path relative to `dir` (NULL means the current directory for
 *  relative paths, "/" for absolute ones).  Returns NULL when not found. */
struct fs_node *vfs_lookup(struct fs_node *dir, const char *path);

/** Like vfs_lookup but resolves the parent directory and final component.
 *  Used by create/unlink/mkdir. */
int vfs_lookup_parent(const char *path, struct fs_node **parent_out, const char *name_out);

int vfs_read(struct fs_node *node, u32 offset, void *buf, u32 len);
int vfs_write(struct fs_node *node, u32 offset, const void *buf, u32 len);
int vfs_readdir(struct fs_node *dir, u32 index, char *name, u32 name_len,
                u32 *type_out, u32 *size_out);
int vfs_mkdir(const char *path, u32 mode, struct fs_node **dir_out);
struct fs_node *vfs_create(const char *path, u32 type, u32 mode);
int vfs_unlink(const char *path);
int vfs_truncate(struct fs_node *node, u32 size);

/** Allocate a fresh inode number. */
u32 vfs_new_inode(void);

/* Directory helpers used by the filesystem implementations. */
int fs_is_dir(struct fs_node *node);

#endif /* LUME_FS_H */
