/* /bin/fstest: file system calls from ring 3. Exits 0 if every check passes,
 * otherwise with the number of the first failed check. */
#include "tern.h"

static int check_no;
#define CHECK(c) do { check_no++; if (!(c)) { \
    printf("fstest: check %d failed: %s\n", check_no, #c); exit(check_no); } } while (0)

int main(void) {
    char buf[256];
    struct tern_stat st;

    /* read a file from the initrd copy */
    int fd = open("/etc/motd", O_RDONLY);
    CHECK(fd == 3);                                  /* 0..2 are the console */
    long n = read(fd, buf, sizeof buf - 1);
    CHECK(n > 0);
    buf[n] = 0;
    CHECK(fstat(fd, &st) == 0 && st.type == TERN_FT_FILE && st.size == (uint64_t)n);
    CHECK(close(fd) == 0);
    CHECK(close(fd) == -EBADF);

    /* create, write, seek, read back */
    CHECK(mkdir("/home") == 0);
    CHECK(mkdir("/home") == -EEXIST);
    fd = open("/home/notes.txt", O_RDWR | O_CREAT);
    CHECK(fd >= 3);
    CHECK(write(fd, "ring three ", 11) == 11);
    CHECK(write(fd, "writes files", 12) == 12);
    CHECK(lseek(fd, 5, SEEK_SET) == 5);
    CHECK(read(fd, buf, 5) == 5 && memcmp(buf, "three", 5) == 0);
    CHECK(lseek(fd, 0, SEEK_END) == 23);
    CHECK(close(fd) == 0);
    CHECK(stat("/home/notes.txt", &st) == 0 && st.size == 23);
    CHECK(stat("/home", &st) == 0 && st.type == TERN_FT_DIR && st.nlink == 1);

    /* readdir finds what we made */
    fd = open("/", O_RDONLY | O_DIRECTORY);
    CHECK(fd >= 3);
    struct tern_dirent d;
    int found = 0, entries = 0;
    while (readdir(fd, &d) == 1) {
        entries++;
        if (strcmp(d.name, "home") == 0 && d.type == TERN_FT_DIR) found++;
        if (strcmp(d.name, "etc") == 0 && d.type == TERN_FT_DIR) found++;
        if (strcmp(d.name, "dev") == 0 && d.type == TERN_FT_DIR) found++;
    }
    CHECK(found == 3 && entries >= 4);
    CHECK(read(fd, buf, 1) == -EISDIR);
    CHECK(close(fd) == 0);

    /* errors */
    CHECK(open("/no/such", O_RDONLY) == -ENOENT);
    CHECK(open("/home", O_WRONLY) == -EISDIR);
    CHECK(open((const char *)0xFFFFFFFF80100000ull, O_RDONLY) == -EFAULT);
    CHECK(open((const char *)0x1000, O_RDONLY) == -EFAULT);
    fd = open("/home/notes.txt", O_RDONLY);
    CHECK(read(fd, (void *)0xFFFFFFFF80100000ull, 4) == -EFAULT);
    CHECK(read(fd, (void *)0x400000, 4) == -EFAULT);           /* our code is read-only */
    CHECK(write(fd, "x", 1) == -EBADF);                          /* opened read-only */
    CHECK(unlink("/home") == -ENOTEMPTY);
    /* the fd table fills up at 16 */
    int fds[16], k = 0;
    while (k < 16 && (fds[k] = open("/etc/motd", O_RDONLY)) >= 0) k++;
    CHECK(k == 16 - 4 && fds[k] == -EMFILE);
    while (k--) CHECK(close(fds[k]) == 0);
    CHECK(close(fd) == 0);

    /* console as a file */
    fd = open("/dev/console", O_WRONLY);
    CHECK(fd >= 3);
    const char msg[] = "[TEST] user fs ok: open/read/write/lseek/readdir/stat/unlink from ring 3\n";
    CHECK(unlink("/home/notes.txt") == 0 && unlink("/home") == 0);
    CHECK(stat("/home", &st) == -ENOENT);
    CHECK(write(fd, msg, sizeof msg - 1) == (long)sizeof msg - 1);
    CHECK(close(fd) == 0);
    return 0;
}
