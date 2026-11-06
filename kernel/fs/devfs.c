/* Device nodes in /dev: console (write = text console + serial; read = the TTY line
 * discipline, tty.c), null and zero. */
#include "vfs.h"
#include "console.h"
#include "string.h"
#include "kernel.h"
#include "tty.h"

static long console_wr(struct vnode *v, const void *buf, size_t len, uint64_t off) {
    (void)v; (void)off;
    const char *s = buf;
    char chunk[129];
    for (size_t done = 0; done < len;) {
        size_t n = len - done > 128 ? 128 : len - done;
        memcpy(chunk, s + done, n);
        chunk[n] = 0;
        console_write(chunk);
        done += n;
    }
    return (long)len;
}
static long eof_rd(struct vnode *v, void *buf, size_t len, uint64_t off) {
    (void)v; (void)buf; (void)len; (void)off;
    return 0;
}
static long console_rd(struct vnode *v, void *buf, size_t len, uint64_t off) {
    (void)v; (void)off;
    return tty_read(buf, len);
}
static long sink_wr(struct vnode *v, const void *buf, size_t len, uint64_t off) {
    (void)v; (void)buf; (void)off;
    return (long)len;
}
static long zero_rd(struct vnode *v, void *buf, size_t len, uint64_t off) {
    (void)v; (void)off;
    memset(buf, 0, len);
    return (long)len;
}

static const struct vnode_ops console_ops = { console_rd, console_wr };
static const struct vnode_ops null_ops = { eof_rd, sink_wr };
static const struct vnode_ops zero_ops = { zero_rd, sink_wr };

static void add_dev(const char *path, const struct vnode_ops *ops) {
    int err = 0;
    struct vnode *v = vfs_create(NULL, path, VN_DEV, &err);
    if (!v) panic("devfs: creating %s failed (%d)", path, err);
    v->ops = ops;
    vfs_put(v);
}

void devfs_init(void) {
    int rc = vfs_mkdir(NULL, "/dev");
    if (rc) panic("devfs: mkdir /dev failed (%d)", rc);
    add_dev("/dev/console", &console_ops);
    add_dev("/dev/null", &null_ops);
    add_dev("/dev/zero", &zero_ops);
}
