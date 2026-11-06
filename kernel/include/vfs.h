#ifndef TERN_VFS_H
#define TERN_VFS_H
/* Virtual file system (Day 18).
 *
 * One tree of vnodes rooted at "/". Directories and regular files live in the RAM
 * file system (ramfs.c): file contents are kmalloc'd buffers that grow on write.
 * Device nodes (e.g. /dev/console) carry their own read/write operations.
 * The initrd is copied into the tree at boot, so everything is writable.
 *
 * Lifetime: a vnode is referenced by its parent directory (while linked) and by every
 * open file. It is freed when it is unlinked and the last reference is dropped.
 * All tree operations take one global mutex (vfs_lock): simple and correct on our
 * single CPU with preemptible system calls. */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "syscall_nr.h"      /* O_* flags, struct tern_stat, struct tern_dirent */

#define VFS_NAME_MAX  55     /* longest path component (bytes, excluding NUL) */
#define VFS_PATH_MAX  256

enum vnode_type { VN_FILE = TERN_FT_FILE, VN_DIR = TERN_FT_DIR, VN_DEV = TERN_FT_DEV };

struct vnode;
struct vnode_ops {                   /* device nodes only; files/dirs use ramfs */
    long (*read)(struct vnode *v, void *buf, size_t len, uint64_t off);
    long (*write)(struct vnode *v, const void *buf, size_t len, uint64_t off);
};

struct vnode {
    enum vnode_type type;
    char name[VFS_NAME_MAX + 1];
    uint32_t refs;                   /* parent link + open files + temporary holds */
    bool linked;                     /* still in the tree */
    uint64_t ino;
    struct vnode *parent;            /* NULL for the root (and once unlinked) */
    struct vnode *children, *sibling;/* directories: singly-linked child list */
    uint32_t nchildren;
    uint8_t *data;                   /* files: contents [0, size), capacity cap */
    uint64_t size, cap;
    const struct vnode_ops *ops;     /* devices */
};

/* An open file description (shared by dup'ed fds in the future). */
struct file {
    struct vnode *node;
    uint64_t off;
    int flags;                       /* O_ACCMODE bits, O_APPEND */
    uint32_t refs;
};

void vfs_init(void);
void devfs_init(void);           /* /dev/console, /dev/null, /dev/zero */
struct vnode *vfs_root(void);

/* Path lookup relative to `cwd` (NULL = root) unless the path is absolute. Returns
 * a held vnode (release with vfs_put) or NULL with *err set (negative errno). */
struct vnode *vfs_lookup(struct vnode *cwd, const char *path, int *err);
void vfs_hold(struct vnode *v);
/* Absolute path of a directory into buf; length or -ERANGE / -ENOENT (unlinked). */
int vfs_getpath(struct vnode *v, char *buf, size_t size);
void vfs_put(struct vnode *v);

/* Create a file/dir/device; returns the new held vnode or NULL with *err.
 * Fails with -EEXIST if the name exists. */
struct vnode *vfs_create(struct vnode *cwd, const char *path, enum vnode_type type, int *err);
int vfs_mkdir(struct vnode *cwd, const char *path);
int vfs_unlink(struct vnode *cwd, const char *path);   /* files, devices, empty dirs */
int vfs_stat(struct vnode *v, struct tern_stat *st);

/* Open files. */
int vfs_open(struct vnode *cwd, const char *path, int flags, struct file **out);
struct file *vfs_open_node(struct vnode *v, int flags);   /* takes its own reference */
void file_hold(struct file *f);
void file_close(struct file *f);
long file_read(struct file *f, void *buf, size_t len);
long file_write(struct file *f, const void *buf, size_t len);
long file_seek(struct file *f, int64_t off, int whence);
/* Directory entry number f->off; 1 and advances on success, 0 at the end. */
int file_readdir(struct file *f, struct tern_dirent *d);

/* Whole-file helpers for kernel users (ELF loader, tests). */
long vfs_write_file(const char *path, const void *data, size_t len);  /* create/trunc */

/* Copy every initrd file into the tree (creating directories). Returns files copied. */
int vfs_populate_from_initrd(void);

struct vfs_stats { uint64_t nodes, file_bytes; };
void vfs_get_stats(struct vfs_stats *s);

void test_vfs(void);

#endif
