/* Pipes (Day 10 session): an anonymous device vnode with a ring buffer. The read end
 * is an O_RDONLY file on it, the write end an O_WRONLY file; when the last reference
 * to an end's file goes away (vnode_ops.close) the other side is woken so it sees end
 * of file / EPIPE. The vnode (and the buffer) is freed when both ends are closed.
 *
 * All pipe state is changed with interrupts off (single CPU), which also makes the
 * check-then-sleep in the blocking paths atomic. Readers and writers share one wait
 * queue; every state change wakes everyone and they re-check. */
#include "vfs.h"
#include "sched.h"
#include "idt.h"
#include "proc.h"
#include "heap.h"
#include "string.h"
#include "kernel.h"

struct pipe {
    uint8_t buf[PIPE_BUF_SIZE];
    size_t rpos, count;
    unsigned readers, writers;     /* open file descriptions per end (0 or 1 each) */
    struct waitq wq;
};

static struct pipe_stats stats;

static bool is_reader(const struct file *f) { return (f->flags & O_ACCMODE) == O_RDONLY; }

static long pipe_fread(struct file *f, void *buf, size_t len) {
    struct pipe *p = f->node->priv;
    if (len == 0) return 0;
    uint64_t fl = irq_save();
    long rc;
    for (;;) {
        if (p->count) {
            size_t n = len < p->count ? len : p->count;
            size_t first = PIPE_BUF_SIZE - p->rpos;
            if (first > n) first = n;
            memcpy(buf, p->buf + p->rpos, first);
            memcpy((uint8_t *)buf + first, p->buf, n - first);
            p->rpos = (p->rpos + n) % PIPE_BUF_SIZE;
            p->count -= n;
            stats.bytes += n;
            waitq_wake_all(&p->wq);
            rc = (long)n;
            break;
        }
        if (!p->writers) { rc = 0; break; }            /* end of file */
        if (proc_kill_pending()) { rc = -EINTR; break; }
        stats.blocked_reads++;
        waitq_sleep_intr(&p->wq);
    }
    irq_restore(fl);
    return rc;
}

static long pipe_fwrite(struct file *f, const void *buf, size_t len) {
    struct pipe *p = f->node->priv;
    const uint8_t *src = buf;
    size_t done = 0;
    uint64_t fl = irq_save();
    long rc = 0;
    while (done < len) {
        if (!p->readers) { stats.epipes++; rc = -EPIPE; break; }
        size_t space = PIPE_BUF_SIZE - p->count;
        if (space) {
            size_t n = len - done < space ? len - done : space;
            size_t wpos = (p->rpos + p->count) % PIPE_BUF_SIZE;
            size_t first = PIPE_BUF_SIZE - wpos;
            if (first > n) first = n;
            memcpy(p->buf + wpos, src + done, first);
            memcpy(p->buf, src + done + first, n - first);
            p->count += n;
            done += n;
            waitq_wake_all(&p->wq);
            continue;
        }
        if (proc_kill_pending()) { rc = -EINTR; break; }
        stats.blocked_writes++;
        waitq_sleep_intr(&p->wq);
    }
    irq_restore(fl);
    return done ? (long)done : rc;
}

static void pipe_close(struct vnode *v, struct file *f) {
    struct pipe *p = v->priv;
    uint64_t fl = irq_save();
    if (is_reader(f)) p->readers--; else p->writers--;
    waitq_wake_all(&p->wq);
    irq_restore(fl);
}

static void pipe_release(struct vnode *v) {
    struct pipe *p = v->priv;
    kassert(!p->readers && !p->writers && !p->wq.head);
    kfree(p);
    v->priv = NULL;
    stats.live--;
}

static const struct vnode_ops pipe_ops = {
    .fread = pipe_fread, .fwrite = pipe_fwrite, .close = pipe_close, .release = pipe_release,
};

int pipe_create(struct file **rd, struct file **wr) {
    struct pipe *p = kzalloc(sizeof *p);
    if (!p) return -ENOMEM;
    waitq_init(&p->wq);
    struct vnode *v = vfs_new_anon(&pipe_ops, p);
    if (!v) { kfree(p); return -ENOMEM; }
    stats.live++;
    stats.created++;
    struct file *r = vfs_open_node(v, O_RDONLY);
    if (r) p->readers = 1;
    struct file *w = r ? vfs_open_node(v, O_WRONLY) : NULL;
    if (w) p->writers = 1;
    vfs_put(v);                           /* the files hold it now */
    if (!w) {
        if (r) file_close(r);             /* frees v and p */
        return -ENOMEM;
    }
    *rd = r;
    *wr = w;
    return 0;
}

void pipe_get_stats(struct pipe_stats *s) {
    uint64_t fl = irq_save();
    *s = stats;
    irq_restore(fl);
}
