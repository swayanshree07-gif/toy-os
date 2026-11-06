/* Boot-time tests for the VFS + RAM FS (Day 18). */
#include "vfs.h"
#include "initrd.h"
#include "heap.h"
#include "string.h"
#include "kernel.h"
#include "kprintf.h"
#include "ktest.h"

#define CHECK(c) do { if (!(c)) panic("vfs test: %s (line %d)", #c, __LINE__); } while (0)

static void expect_content(const char *path, const void *want, size_t n) {
    struct file *f;
    CHECK(vfs_open(NULL, path, O_RDONLY, &f) == 0);
    char *buf = kmalloc(n + 16);
    CHECK(buf);
    long got = file_read(f, buf, n + 16);
    if (got != (long)n || memcmp(buf, want, n)) panic("vfs test: %s: read %ld, want %zu", path, got, n);
    CHECK(file_read(f, buf, 1) == 0);           /* end of file */
    kfree(buf);
    file_close(f);
}

void test_vfs(void) {
    struct vfs_stats s0, s1;
    vfs_get_stats(&s0);

    /* the initrd copy */
    const struct initrd *rd = initrd_boot();
    struct initrd_file motd;
    CHECK(rd && initrd_find(rd, "etc/motd", &motd));
    expect_content("/etc/motd", motd.data, motd.size);
    expect_content("/etc/../etc/./motd", motd.data, motd.size);

    /* files: write, read back, seek, append, holes, truncate */
    CHECK(vfs_mkdir(NULL, "/tmp") == 0);
    CHECK(vfs_mkdir(NULL, "/tmp") == -EEXIST);
    struct file *f;
    CHECK(vfs_open(NULL, "/tmp/a.txt", O_RDWR, &f) == -ENOENT);
    CHECK(vfs_open(NULL, "/tmp/a.txt", O_RDWR | O_CREAT, &f) == 0);
    CHECK(file_write(f, "hello ", 6) == 6);
    CHECK(file_write(f, "world", 5) == 5);
    CHECK(file_seek(f, 0, SEEK_SET) == 0);
    char b[64];
    CHECK(file_read(f, b, sizeof b) == 11 && memcmp(b, "hello world", 11) == 0);
    CHECK(file_seek(f, -5, SEEK_END) == 6);
    CHECK(file_write(f, "WORLD!", 6) == 6);
    CHECK(file_seek(f, 20, SEEK_SET) == 20);
    CHECK(file_write(f, "x", 1) == 1);              /* hole 12..19 reads as zeros */
    CHECK(file_seek(f, -1, SEEK_SET) == -EINVAL);
    file_close(f);
    const char want[] = "hello WORLD!\0\0\0\0\0\0\0\0x";
    expect_content("/tmp/a.txt", want, sizeof want - 1);
    CHECK(vfs_open(NULL, "/tmp/a.txt", O_WRONLY | O_APPEND, &f) == 0);
    CHECK(file_write(f, "yz", 2) == 2);
    CHECK(file_read(f, b, 1) == -EBADF);             /* write-only */
    file_close(f);
    CHECK(vfs_open(NULL, "/tmp/a.txt", O_RDONLY, &f) == 0);
    CHECK(file_write(f, "q", 1) == -EBADF);          /* read-only */
    file_close(f);
    CHECK(vfs_write_file("/tmp/a.txt", "short", 5) == 5);    /* O_TRUNC */
    expect_content("/tmp/a.txt", "short", 5);

    /* a big file grows through several reallocations */
    static uint8_t pat[1000];
    for (unsigned i = 0; i < sizeof pat; i++) pat[i] = (uint8_t)(i * 7 + 3);
    CHECK(vfs_open(NULL, "/tmp/big", O_WRONLY | O_CREAT, &f) == 0);
    for (int i = 0; i < 100; i++) CHECK(file_write(f, pat, sizeof pat) == (long)sizeof pat);
    file_close(f);
    CHECK(vfs_open(NULL, "/tmp/big", O_RDONLY, &f) == 0);
    for (int i = 0; i < 100; i++) {
        static uint8_t rb[1000];
        CHECK(file_read(f, rb, sizeof rb) == (long)sizeof rb && memcmp(rb, pat, sizeof pat) == 0);
    }
    file_close(f);

    /* directories: nesting, relative lookup, readdir order, errors */
    CHECK(vfs_mkdir(NULL, "/tmp/d") == 0);
    CHECK(vfs_mkdir(NULL, "/tmp/d/e") == 0);
    int err = 0;
    struct vnode *d = vfs_lookup(NULL, "/tmp/d", &err);
    CHECK(d);
    CHECK(vfs_write_file("/tmp/d/f1", "1", 1) == 1);
    struct vnode *v = vfs_create(d, "f2", VN_FILE, &err);   /* relative to d */
    CHECK(v);
    vfs_put(v);
    v = vfs_lookup(d, "../d/e/..//f2", &err);
    CHECK(v && v->type == VN_FILE);
    vfs_put(v);
    CHECK(vfs_open(d, "f1/x", O_RDONLY, &f) == -ENOTDIR);
    CHECK(vfs_open(NULL, "/tmp/d", O_WRONLY, &f) == -EISDIR);
    CHECK(vfs_open(NULL, "/tmp/d/f1", O_RDONLY | O_DIRECTORY, &f) == -ENOTDIR);
    CHECK(vfs_lookup(NULL, "/nope/x", &err) == NULL && err == -ENOENT);
    CHECK(vfs_open(NULL, "/tmp/d", O_RDONLY | O_DIRECTORY, &f) == 0);
    struct tern_dirent de;
    const char *order[] = { "e", "f1", "f2" };
    for (int i = 0; i < 3; i++) {
        CHECK(file_readdir(f, &de) == 1);
        if (strcmp(de.name, order[i])) panic("vfs test: readdir %d = '%s'", i, de.name);
    }
    CHECK(file_readdir(f, &de) == 0);
    CHECK(file_read(f, b, 1) == -EISDIR);
    file_close(f);
    char longname[80];
    memset(longname, 'n', sizeof longname);
    longname[0] = '/';
    longname[79] = 0;
    CHECK(vfs_mkdir(NULL, longname) == -ENAMETOOLONG);

    /* unlink: open files stay readable until closed */
    CHECK(vfs_unlink(NULL, "/tmp/d") == -ENOTEMPTY);
    CHECK(vfs_open(NULL, "/tmp/d/f1", O_RDONLY, &f) == 0);
    CHECK(vfs_unlink(NULL, "/tmp/d/f1") == 0);
    CHECK(vfs_lookup(NULL, "/tmp/d/f1", &err) == NULL && err == -ENOENT);
    CHECK(file_read(f, b, 8) == 1 && b[0] == '1');
    file_close(f);
    CHECK(vfs_unlink(d, "f2") == 0);
    CHECK(vfs_unlink(NULL, "/tmp/d/e") == 0);
    CHECK(vfs_unlink(NULL, "/tmp/d") == 0);
    CHECK(vfs_lookup(d, "x", &err) == NULL && err == -ENOENT);   /* cwd removed */
    vfs_put(d);
    CHECK(vfs_unlink(NULL, "/") == -EACCES);

    /* devices */
    CHECK(vfs_open(NULL, "/dev/zero", O_RDONLY, &f) == 0);
    memset(b, 1, 8);
    CHECK(file_read(f, b, 8) == 8 && b[0] == 0 && b[7] == 0);
    file_close(f);
    CHECK(vfs_open(NULL, "/dev/null", O_RDWR, &f) == 0);
    CHECK(file_write(f, "gone", 4) == 4 && file_read(f, b, 4) == 0);
    file_close(f);

    CHECK(vfs_unlink(NULL, "/tmp/a.txt") == 0);
    CHECK(vfs_unlink(NULL, "/tmp/big") == 0);
    CHECK(vfs_unlink(NULL, "/tmp") == 0);
    vfs_get_stats(&s1);
    if (s1.nodes != s0.nodes || s1.file_bytes != s0.file_bytes)
        panic("vfs test: leak: nodes %lu -> %lu, bytes %lu -> %lu",
              s0.nodes, s1.nodes, s0.file_bytes, s1.file_bytes);
    kprintf("[TEST] ramfs ok: files/dirs/devices, seek+append+holes, unlink while open, "
            "%lu nodes, no leaks\n", s1.nodes);
}

KTEST(vfs_path_edge_cases) {
    int err = 0;
    struct vnode *v = vfs_lookup(NULL, "/", &err);
    KEXPECT(v == vfs_root());
    vfs_put(v);
    v = vfs_lookup(NULL, "/..", &err);
    KEXPECT(v == vfs_root());
    vfs_put(v);
    v = vfs_lookup(NULL, "///dev//console", &err);
    KEXPECT(v && v->type == VN_DEV);
    vfs_put(v);
    KEXPECT(vfs_lookup(NULL, "", &err) == NULL && err == -ENOENT);
    KEXPECT(vfs_mkdir(NULL, "/") == -EEXIST);
    KEXPECT(vfs_mkdir(NULL, "/dev/..") == -EEXIST);
    KEXPECT(vfs_create(NULL, "/dev/console/x", VN_FILE, &err) == NULL && err == -ENOTDIR);
}

KTEST(vfs_getpath) {
    char buf[32];
    int err = 0;
    KEXPECT(vfs_getpath(vfs_root(), buf, sizeof buf) == 1 && !strcmp(buf, "/"));
    KEXPECT(vfs_mkdir(NULL, "/gp") == 0 && vfs_mkdir(NULL, "/gp/inner") == 0);
    struct vnode *v = vfs_lookup(NULL, "/gp/./inner/../inner", &err);
    KEXPECT(v != NULL);
    KEXPECT(vfs_getpath(v, buf, sizeof buf) == 9 && !strcmp(buf, "/gp/inner"));
    KEXPECT(vfs_getpath(v, buf, 10) == 9);                /* exactly fits */
    KEXPECT(vfs_getpath(v, buf, 9) == -ERANGE);
    KEXPECT(vfs_unlink(NULL, "/gp/inner") == 0);
    KEXPECT(vfs_getpath(v, buf, sizeof buf) == -ENOENT);  /* removed while held */
    vfs_put(v);
    KEXPECT(vfs_unlink(NULL, "/gp") == 0);
}
