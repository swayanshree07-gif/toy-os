/* VFS tree + RAM file system (see vfs.h). */
#include "vfs.h"
#include "initrd.h"
#include "sync.h"
#include "heap.h"
#include "string.h"
#include "kernel.h"
#include "kprintf.h"

static struct mutex vfs_lock;
static struct vnode *root;
static uint64_t next_ino = 1;
static uint64_t live_nodes, file_bytes;

static struct vnode *node_new(enum vnode_type type, const char *name, size_t nlen) {
    struct vnode *v = kzalloc(sizeof *v);
    if (!v) return NULL;
    v->type = type;
    memcpy(v->name, name, nlen);
    v->name[nlen] = 0;
    v->ino = next_ino++;
    live_nodes++;
    return v;
}

static void node_free(struct vnode *v) {
    kassert(v->refs == 0 && !v->linked && !v->children);
    if (v->ops && v->ops->release) v->ops->release(v);
    file_bytes -= v->cap;
    kfree(v->data);
    kfree(v);
    live_nodes--;
}

/* Reference counting, called with vfs_lock held. */
static void hold_locked(struct vnode *v) { v->refs++; }
static void put_locked(struct vnode *v) {
    kassert(v->refs > 0);
    if (--v->refs == 0) node_free(v);
}

void vfs_hold(struct vnode *v) {
    mutex_lock(&vfs_lock);
    hold_locked(v);
    mutex_unlock(&vfs_lock);
}
void vfs_put(struct vnode *v) {
    if (!v) return;
    mutex_lock(&vfs_lock);
    put_locked(v);
    mutex_unlock(&vfs_lock);
}

void vfs_init(void) {
    mutex_init(&vfs_lock);
    root = node_new(VN_DIR, "", 0);
    kassert(root);
    root->refs = 1;           /* the tree itself */
    root->linked = true;
}

struct vnode *vfs_root(void) { return root; }

static struct vnode *dir_find(struct vnode *d, const char *name, size_t nlen) {
    for (struct vnode *c = d->children; c; c = c->sibling)
        if (strlen(c->name) == nlen && memcmp(c->name, name, nlen) == 0) return c;
    return NULL;
}

static void dir_link(struct vnode *d, struct vnode *c) {
    /* append, so readdir lists entries in creation order */
    struct vnode **pp = &d->children;
    while (*pp) pp = &(*pp)->sibling;
    *pp = c;
    c->sibling = NULL;
    c->parent = d;
    c->linked = true;
    c->refs++;
    d->nchildren++;
}

static void dir_unlink(struct vnode *d, struct vnode *c) {
    struct vnode **pp = &d->children;
    while (*pp != c) pp = &(*pp)->sibling;
    *pp = c->sibling;
    c->sibling = NULL;
    c->parent = NULL;
    c->linked = false;
    d->nchildren--;
    put_locked(c);
}

/* Walk `path` from cwd. If `parent_mode`, stops before the last component: returns the
 * (unheld) parent directory and sets *last and *lastlen. Otherwise returns the (unheld)
 * node. Called with vfs_lock held. */
static struct vnode *walk(struct vnode *cwd, const char *path, int *err, bool parent_mode,
                          const char **last, size_t *lastlen) {
    if (!path || !path[0]) { *err = -ENOENT; return NULL; }
    if (strlen(path) >= VFS_PATH_MAX) { *err = -ENAMETOOLONG; return NULL; }
    struct vnode *cur = (path[0] == '/' || !cwd) ? root : cwd;
    if (!cur->linked) { *err = -ENOENT; return NULL; }   /* cwd was removed */
    const char *p = path;
    for (;;) {
        while (*p == '/') p++;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - s);
        const char *q = p;
        while (*q == '/') q++;
        bool final = (*q == 0);
        if (n == 0) {                       /* path was "/" or ended in slashes */
            if (parent_mode) { *err = -EEXIST; return NULL; }
            return cur;
        }
        if (n > VFS_NAME_MAX) { *err = -ENAMETOOLONG; return NULL; }
        if (cur->type != VN_DIR) { *err = -ENOTDIR; return NULL; }
        if (final && parent_mode) {
            if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) {
                *err = -EEXIST; return NULL;
            }
            *last = s; *lastlen = n;
            return cur;
        }
        struct vnode *next;
        if (n == 1 && s[0] == '.') next = cur;
        else if (n == 2 && s[0] == '.' && s[1] == '.') next = cur->parent ? cur->parent : cur;
        else next = dir_find(cur, s, n);
        if (!next) { *err = -ENOENT; return NULL; }
        cur = next;
        if (final) return cur;
    }
}

int vfs_getpath(struct vnode *v, char *buf, size_t size) {
    if (size < 2) return -ERANGE;
    mutex_lock(&vfs_lock);
    int rc = 0;
    if (!v->linked) { rc = -ENOENT; goto out; }
    /* build right to left at the end of buf, then move it to the front */
    size_t pos = size - 1;
    buf[pos] = 0;
    for (struct vnode *c = v; c != root; c = c->parent) {
        size_t n = strlen(c->name);
        if (n + 1 > pos) { rc = -ERANGE; goto out; }
        pos -= n;
        memcpy(buf + pos, c->name, n);
        buf[--pos] = '/';
    }
    if (pos == size - 1) buf[--pos] = '/';
    rc = (int)(size - 1 - pos);
    memmove(buf, buf + pos, (size_t)rc + 1);
out:
    mutex_unlock(&vfs_lock);
    return rc;
}

struct vnode *vfs_lookup(struct vnode *cwd, const char *path, int *err) {
    mutex_lock(&vfs_lock);
    struct vnode *v = walk(cwd, path, err, false, NULL, NULL);
    if (v) hold_locked(v);
    mutex_unlock(&vfs_lock);
    return v;
}

static struct vnode *create_locked(struct vnode *cwd, const char *path,
                                   enum vnode_type type, int *err) {
    const char *name;
    size_t nlen;
    struct vnode *d = walk(cwd, path, err, true, &name, &nlen);
    if (!d) return NULL;
    if (d->type != VN_DIR) { *err = -ENOTDIR; return NULL; }
    if (dir_find(d, name, nlen)) { *err = -EEXIST; return NULL; }
    struct vnode *v = node_new(type, name, nlen);
    if (!v) { *err = -ENOMEM; return NULL; }
    if (d->ops && d->ops->create) {
        int rc = d->ops->create(d, v);
        if (rc) { node_free(v); *err = rc; return NULL; }
    }
    dir_link(d, v);
    hold_locked(v);
    return v;
}

struct vnode *vfs_create(struct vnode *cwd, const char *path, enum vnode_type type, int *err) {
    mutex_lock(&vfs_lock);
    struct vnode *v = create_locked(cwd, path, type, err);
    mutex_unlock(&vfs_lock);
    return v;
}

int vfs_mkdir(struct vnode *cwd, const char *path) {
    int err = 0;
    struct vnode *v = vfs_create(cwd, path, VN_DIR, &err);
    if (!v) return err;
    vfs_put(v);
    return 0;
}

int vfs_unlink(struct vnode *cwd, const char *path) {
    int err = 0;
    mutex_lock(&vfs_lock);
    struct vnode *v = walk(cwd, path, &err, false, NULL, NULL);
    if (v && v == root) err = -EACCES, v = NULL;
    if (v && v->type == VN_DIR && v->children) err = -ENOTEMPTY, v = NULL;
    if (v && v->parent->ops && v->parent->ops->unlink) {
        err = v->parent->ops->unlink(v->parent, v);
        if (err) v = NULL;
    }
    if (v) dir_unlink(v->parent, v);
    mutex_unlock(&vfs_lock);
    return v ? 0 : err;
}

int vfs_rename(struct vnode *cwd, const char *from, const char *to) {
    int err = 0;
    const char *name;
    size_t nlen;
    mutex_lock(&vfs_lock);
    struct vnode *src = walk(cwd, from, &err, false, NULL, NULL);
    struct vnode *d = src ? walk(cwd, to, &err, true, &name, &nlen) : NULL;
    if (!d) goto out;
    if (src == root || !src->parent) { err = -EBUSY; goto out; }
    if (d->type != VN_DIR) { err = -ENOTDIR; goto out; }
    if ((d->ops && d->ops->create) || (src->parent->ops && src->parent->ops->unlink)) {
        err = -EXDEV; goto out;                     /* disk directories: no rename */
    }
    if (src->type == VN_DIR)                        /* not into its own subtree */
        for (struct vnode *a = d; a; a = a->parent)
            if (a == src) { err = -EINVAL; goto out; }
    struct vnode *old = dir_find(d, name, nlen);
    if (old == src) goto out;                       /* same name: nothing to do */
    if (old) {
        if (old->type == VN_DIR && src->type != VN_DIR) { err = -EISDIR; goto out; }
        if (old->type != VN_DIR && src->type == VN_DIR) { err = -ENOTDIR; goto out; }
        if (old->type == VN_DIR && old->children) { err = -ENOTEMPTY; goto out; }
        dir_unlink(d, old);
    }
    hold_locked(src);
    dir_unlink(src->parent, src);
    memcpy(src->name, name, nlen);
    src->name[nlen] = 0;
    dir_link(d, src);
    put_locked(src);
out:
    mutex_unlock(&vfs_lock);
    return err;
}

struct vnode *vfs_new_anon(const struct vnode_ops *ops, void *priv) {
    mutex_lock(&vfs_lock);
    struct vnode *v = node_new(VN_DEV, "", 0);
    if (v) { v->ops = ops; v->priv = priv; v->refs = 1; }
    mutex_unlock(&vfs_lock);
    return v;
}

int vfs_stat(struct vnode *v, struct tern_stat *st) {
    mutex_lock(&vfs_lock);
    st->type = v->type;
    st->nlink = v->type == VN_DIR ? v->nchildren : 1;
    st->size = v->size;
    st->ino = v->ino;
    mutex_unlock(&vfs_lock);
    return 0;
}

/* ---- open files ---- */

static struct file *file_new(struct vnode *v, int flags) {
    struct file *f = kzalloc(sizeof *f);
    if (!f) return NULL;
    f->node = v;
    f->flags = flags & (O_ACCMODE | O_APPEND);
    f->refs = 1;
    return f;
}

struct file *vfs_open_node(struct vnode *v, int flags) {
    struct file *f = file_new(v, flags);
    if (f) vfs_hold(v);
    return f;
}

static bool can_read(const struct file *f) { return (f->flags & O_ACCMODE) != O_WRONLY; }
static bool can_write(const struct file *f) {
    int m = f->flags & O_ACCMODE;
    return m == O_WRONLY || m == O_RDWR;
}

int vfs_open(struct vnode *cwd, const char *path, int flags, struct file **out) {
    int err = 0;
    if ((flags & O_ACCMODE) == 3) return -EINVAL;
    mutex_lock(&vfs_lock);
    struct vnode *v = walk(cwd, path, &err, false, NULL, NULL);
    if (v) {
        hold_locked(v);
    } else if (err == -ENOENT && (flags & O_CREAT)) {
        v = create_locked(cwd, path, VN_FILE, &err);
    }
    if (!v) { mutex_unlock(&vfs_lock); return err; }
    err = 0;
    if (v->type == VN_DIR && (flags & O_ACCMODE) != O_RDONLY) err = -EISDIR;
    else if (v->type != VN_DIR && (flags & O_DIRECTORY)) err = -ENOTDIR;
    if (!err && v->type == VN_FILE && v->ops && !v->ops->write && (flags & O_ACCMODE) != O_RDONLY)
        err = -EROFS;
    if (!err && v->type == VN_FILE && (flags & O_TRUNC) && (flags & O_ACCMODE) != O_RDONLY) {
        if (v->ops && v->ops->truncate) err = v->ops->truncate(v);
        else v->size = 0;
    }
    struct file *f = err ? NULL : file_new(v, flags);
    if (!err && !f) err = -ENOMEM;
    if (err) put_locked(v);
    mutex_unlock(&vfs_lock);
    if (err) return err;
    *out = f;
    return 0;
}

void file_hold(struct file *f) {
    mutex_lock(&vfs_lock);
    f->refs++;
    mutex_unlock(&vfs_lock);
}

void file_close(struct file *f) {
    mutex_lock(&vfs_lock);
    kassert(f->refs > 0);
    bool last = --f->refs == 0;
    if (last && f->node->type == VN_DEV && f->node->ops->close) f->node->ops->close(f->node, f);
    if (last) put_locked(f->node);
    mutex_unlock(&vfs_lock);
    if (last) kfree(f);
}

long file_read(struct file *f, void *buf, size_t len) {
    if (!can_read(f)) return -EBADF;
    struct vnode *v = f->node;
    if (v->type == VN_DIR) return -EISDIR;
    if (v->type == VN_DEV) {
        if (v->ops->fread) return v->ops->fread(f, buf, len);
        if (!v->ops->read) return -EINVAL;
        return v->ops->read(v, buf, len, f->off);
    }
    mutex_lock(&vfs_lock);
    long n = 0;
    if (v->ops && v->ops->read) {               /* backed by a disk file system */
        if (f->off < v->size) {
            uint64_t avail = v->size - f->off;
            n = v->ops->read(v, buf, len < avail ? len : (size_t)avail, f->off);
            if (n > 0) f->off += (uint64_t)n;
        }
    } else if (f->off < v->size) {
        uint64_t avail = v->size - f->off;
        n = (long)(len < avail ? len : avail);
        memcpy(buf, v->data + f->off, (size_t)n);
        f->off += (uint64_t)n;
    }
    mutex_unlock(&vfs_lock);
    return n;
}

#define FILE_MAX_SIZE (64ull << 20)

static int grow(struct vnode *v, uint64_t need) {
    if (need <= v->cap) return 0;
    if (need > FILE_MAX_SIZE) return -ENOSPC;
    uint64_t cap = v->cap ? v->cap : 64;
    while (cap < need) cap *= 2;
    uint8_t *d = kmalloc(cap);
    if (!d) return -ENOMEM;
    if (v->size) memcpy(d, v->data, v->size);
    kfree(v->data);
    file_bytes += cap - v->cap;
    v->data = d;
    v->cap = cap;
    return 0;
}

long file_write(struct file *f, const void *buf, size_t len) {
    if (!can_write(f)) return -EBADF;
    struct vnode *v = f->node;
    if (v->type == VN_DEV) {
        if (v->ops->fwrite) return v->ops->fwrite(f, buf, len);
        if (!v->ops->write) return -EINVAL;
        return v->ops->write(v, buf, len, f->off);
    }
    if (v->type != VN_FILE) return -EISDIR;
    if (v->ops && !v->ops->write) return -EROFS;
    mutex_lock(&vfs_lock);
    if (f->flags & O_APPEND) f->off = v->size;
    long rc;
    if (v->ops) {                                  /* disk file */
        rc = len > FILE_MAX_SIZE || f->off > FILE_MAX_SIZE - len ? -ENOSPC
                                                                 : v->ops->write(v, buf, len, f->off);
        if (rc > 0) f->off += (uint64_t)rc;
    } else if (len > FILE_MAX_SIZE || f->off > FILE_MAX_SIZE - len) {
        rc = -ENOSPC;
    } else if ((rc = grow(v, f->off + len)) == 0) {
        if (f->off > v->size) memset(v->data + v->size, 0, f->off - v->size);  /* hole */
        memcpy(v->data + f->off, buf, len);
        f->off += len;
        if (f->off > v->size) v->size = f->off;
        rc = (long)len;
    }
    mutex_unlock(&vfs_lock);
    return rc;
}

long file_seek(struct file *f, int64_t off, int whence) {
    if (f->node->type == VN_DEV) return -EINVAL;
    mutex_lock(&vfs_lock);
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->off
                 : whence == SEEK_END ? (int64_t)f->node->size : -1;
    long rc;
    if (base < 0 || (whence == SEEK_SET && off < 0)) rc = -EINVAL;
    else if (base + off < 0 || (uint64_t)(base + off) > FILE_MAX_SIZE) rc = -EINVAL;
    else f->off = (uint64_t)(base + off), rc = (long)f->off;
    mutex_unlock(&vfs_lock);
    return rc;
}

int file_readdir(struct file *f, struct tern_dirent *d) {
    struct vnode *v = f->node;
    if (v->type != VN_DIR) return -ENOTDIR;
    mutex_lock(&vfs_lock);
    struct vnode *c = v->children;
    for (uint64_t i = 0; c && i < f->off; i++) c = c->sibling;
    int rc = 0;
    if (c) {
        memset(d, 0, sizeof *d);
        d->type = c->type;
        d->size = c->type == VN_DIR ? c->nchildren : c->size;
        memcpy(d->name, c->name, strlen(c->name) + 1);
        f->off++;
        rc = 1;
    }
    mutex_unlock(&vfs_lock);
    return rc;
}

long vfs_write_file(const char *path, const void *data, size_t len) {
    struct file *f;
    int rc = vfs_open(NULL, path, O_WRONLY | O_CREAT | O_TRUNC, &f);
    if (rc) return rc;
    long n = file_write(f, data, len);
    file_close(f);
    return n;
}

/* mkdir -p for every directory component of `path` (not the last one). */
static int make_parents(const char *path) {
    char buf[VFS_PATH_MAX];
    size_t n = strlen(path);
    if (n >= sizeof buf) return -ENAMETOOLONG;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] != '/') continue;
        buf[i] = 0;
        int rc = vfs_mkdir(NULL, buf);
        if (rc && rc != -EEXIST) return rc;
        buf[i] = '/';
    }
    return 0;
}

int vfs_populate_from_initrd(void) {
    const struct initrd *rd = initrd_boot();
    int copied = 0;
    struct initrd_file f;
    for (uint32_t i = 0; rd && initrd_get(rd, i, &f); i++) {
        char path[VFS_PATH_MAX];
        size_t n = strlen(f.name);
        if (n + 2 > sizeof path) continue;
        path[0] = '/';
        memcpy(path + 1, f.name, n + 1);
        if (make_parents(path)) { kprintf("vfs: cannot create parents of %s\n", path); continue; }
        long w = vfs_write_file(path, f.data, f.size);
        if (w != (long)f.size) { kprintf("vfs: copying %s failed (%ld)\n", path, w); continue; }
        copied++;
    }
    return copied;
}

void vfs_get_stats(struct vfs_stats *s) {
    mutex_lock(&vfs_lock);
    s->nodes = live_nodes;
    s->file_bytes = file_bytes;
    mutex_unlock(&vfs_lock);
}
