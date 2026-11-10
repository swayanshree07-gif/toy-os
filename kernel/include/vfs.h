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
struct file;
/* Device nodes use read/write. A VN_FILE with ops is backed by a disk file system
 * (fat.c): read/write/truncate are called with the VFS lock held, read with len clipped
 * to v->size; write updates v->size itself. Without `write` it is read-only (opening
 * for writing gives -EROFS). A VN_DIR with ops is a disk directory: `create` is called
 * before a new child is linked (and may set child->ops/priv; an error aborts the
 * create), `unlink` before a child is removed. Rename into/out of/inside a disk
 * directory gives -EXDEV. */
struct vnode_ops {
    long (*read)(struct vnode *v, void *buf, size_t len, uint64_t off);
    long (*write)(struct vnode *v, const void *buf, size_t len, uint64_t off);
    /* optional: an open file description on v is closed for the last time (called
     * with the VFS lock held; must not sleep) */
    void (*close)(struct vnode *v, struct file *f);
    /* optional: v is being freed; release v->priv */
    void (*release)(struct vnode *v);
    int (*truncate)(struct vnode *v);                        /* disk files: size -> 0 */
    int (*create)(struct vnode *dir, struct vnode *child);   /* disk directories */
    int (*unlink)(struct vnode *dir, struct vnode *child);
    /* disk nodes: move src's on-disk entry to directory newdir as `name` (Day 24) */
    int (*rename)(struct vnode *src, struct vnode *newdir, const char *name);
    /* optional, used instead of read/write when set: the open file is passed (pipes
     * need to know which end) */
    long (*fread)(struct file *f, void *buf, size_t len);
    long (*fwrite)(struct file *f, const void *buf, size_t len);
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
    void *priv;                      /* devices: driver data (pipes) */
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
/* Move/rename `from` to `to` (POSIX rename semantics: an existing file target is
 * replaced, an existing empty directory is replaced by a directory; a directory can't
 * be moved into its own subtree, -EINVAL). */
int vfs_rename(struct vnode *cwd, const char *from, const char *to);
/* A device vnode that is not in the tree (pipes). Returned held (refs 1). */
struct vnode *vfs_new_anon(const struct vnode_ops *ops, void *priv);
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

/* Pipes (pipe.c): a 4 KiB ring buffer with a read end and a write end. Reads block
 * while it is empty and writers exist (0 = end of file once all writers closed);
 * writes block while it is full and return -EPIPE once all readers closed. Both are
 * interrupted by kill (-EINTR, or the bytes transferred so far). */
#define PIPE_BUF_SIZE 4096
int pipe_create(struct file **rd, struct file **wr);
struct pipe_stats { uint64_t live, created, bytes, blocked_reads, blocked_writes, epipes; };
void pipe_get_stats(struct pipe_stats *s);

/* FAT16 from an AHCI disk, mounted at `path` (fat.c). The directory tree is built in
 * the VFS at mount time; file contents are read from the disk on demand. Writable
 * (Day 16): write/append/truncate, create, mkdir, unlink go straight to the disk.
 * Returns files mounted or a negative errno (-ENODEV: no FAT16 file system on it). */
int fat_mount(int disk, const char *path);
int fat_find_disk(void);       /* first AHCI disk with a FAT16 boot sector, or -ENODEV */
struct fat_stats { uint64_t sector_reads, cache_hits, sector_writes, free_clusters; };
void fat_get_stats(struct fat_stats *s);
/* Day 25. Called with the VFS lock held (through vfs_statfs / vfs_sync). */
bool fat_statfs_locked(struct vnode *v, struct tern_statfs *st);   /* false: not FAT */
int fat_sync_all_locked(void);               /* volumes flushed or -EIO */
const char *fat_mount_of_disk(int disk);     /* mount path or NULL */
int vfs_statfs(struct vnode *v, struct tern_statfs *st);
int vfs_sync(void);

void test_vfs(void);

#endif
