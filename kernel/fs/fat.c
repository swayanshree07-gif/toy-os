/* Read-only FAT16 file system on an AHCI disk (Day 15), see vfs.h.
 *
 * Only "superfloppy" volumes (boot sector at LBA 0, no partition table) with 512-byte
 * sectors. At mount the whole FAT is read into memory and the directory tree is copied
 * into the VFS: every directory becomes a ramfs directory and every file a VN_FILE whose
 * ops read the cluster chain from the disk on demand (so file data is never copied).
 * Only 8.3 names (shown in lower case); long-file-name entries, the volume label and
 * deleted entries are skipped. A one-sector cache serves sequential small reads. */
#include "vfs.h"
#include "ahci.h"
#include "heap.h"
#include "string.h"
#include "kernel.h"
#include "kprintf.h"

struct fatfs {
    int disk;
    uint32_t spc, fat_lba, root_lba, root_ents, data_lba, nclusters;
    uint16_t *fat;                   /* nclusters + 2 entries */
    uint8_t cache[512];
    uint64_t cache_lba;              /* UINT64_MAX = empty */
};
struct fatfile { struct fatfs *fs; uint16_t first; };

static struct fat_stats stats;

void fat_get_stats(struct fat_stats *s) { *s = stats; }

static int read_sector(struct fatfs *fs, uint64_t lba, void *buf) {
    if (lba == fs->cache_lba) { stats.cache_hits++; memcpy(buf, fs->cache, 512); return 0; }
    int r = ahci_read(fs->disk, lba, 1, fs->cache);
    stats.sector_reads++;
    if (r) { fs->cache_lba = UINT64_MAX; return r; }
    fs->cache_lba = lba;
    memcpy(buf, fs->cache, 512);
    return 0;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }

static bool valid_cluster(const struct fatfs *fs, uint32_t c) { return c >= 2 && c < fs->nclusters + 2; }
static uint64_t cluster_lba(const struct fatfs *fs, uint32_t c) { return fs->data_lba + (uint64_t)(c - 2) * fs->spc; }

/* Parse the boot sector; 0 if it is a FAT16 volume we can use. */
static int parse_bpb(const uint8_t *b, struct fatfs *fs) {
    if (b[510] != 0x55 || b[511] != 0xAA) return -ENODEV;
    uint32_t bps = rd16(b + 11), spc = b[13], rsv = rd16(b + 14), nfats = b[16];
    uint32_t rootents = rd16(b + 17), total = rd16(b + 19), fatsz = rd16(b + 22);
    if (!total) total = rd32(b + 32);
    if (bps != 512 || !spc || (spc & (spc - 1)) || !rsv || !nfats || !fatsz || !rootents)
        return -ENODEV;
    uint32_t rootsecs = (rootents * 32 + 511) / 512;
    uint32_t data = rsv + nfats * fatsz + rootsecs;
    if (data >= total) return -ENODEV;
    uint32_t nclus = (total - data) / spc;
    if (nclus < 4085 || nclus >= 65525) return -ENODEV;   /* FAT12 / FAT32 */
    if (fatsz * 256 < nclus + 2) return -ENODEV;          /* FAT too small */
    fs->spc = spc; fs->fat_lba = rsv; fs->root_lba = rsv + nfats * fatsz;
    fs->root_ents = rootents; fs->data_lba = data; fs->nclusters = nclus;
    return 0;
}

/* Copy up to len bytes of the file at offset off (already clipped to the size). */
static long fat_read(struct vnode *v, void *buf, size_t len, uint64_t off) {
    struct fatfile *ff = v->priv;
    struct fatfs *fs = ff->fs;
    uint32_t csize = fs->spc * 512;
    uint32_t c = ff->first;
    for (uint64_t skip = off / csize; skip && valid_cluster(fs, c); skip--) c = fs->fat[c];
    uint8_t sec[512];
    size_t done = 0;
    uint64_t pos = off;
    while (done < len) {
        if (!valid_cluster(fs, c)) return done ? (long)done : -EIO;   /* chain too short */
        uint32_t in = (uint32_t)(pos % csize);
        if (read_sector(fs, cluster_lba(fs, c) + in / 512, sec)) return done ? (long)done : -EIO;
        size_t n = 512 - in % 512;
        if (n > len - done) n = len - done;
        memcpy((uint8_t *)buf + done, sec + in % 512, n);
        done += n; pos += n;
        if (pos % csize == 0) c = fs->fat[c];
    }
    return (long)done;
}

static void fat_release(struct vnode *v) { kfree(v->priv); v->priv = NULL; }

static const struct vnode_ops fat_file_ops = { .read = fat_read, .release = fat_release };

/* "NAME    EXT" -> "name.ext" */
static void short_name(const uint8_t *e, char *out) {
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) out[n++] = (char)e[i];
    if (out[0] == 0x05) out[0] = (char)0xE5;
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) out[n++] = (char)e[i];
    }
    out[n] = 0;
    for (int i = 0; i < n; i++) if (out[i] >= 'A' && out[i] <= 'Z') out[i] += 'a' - 'A';
}

static int mount_dir(struct fatfs *fs, uint32_t clus, const char *path, int depth);

/* Handle one 32-byte entry. 1 = end of directory, 0 = continue, <0 error. */
static int add_entry(struct fatfs *fs, const uint8_t *e, const char *dir, int depth, int *files) {
    if (e[0] == 0) return 1;
    if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08)) return 0;   /* deleted, LFN, label */
    if (e[0] == '.') return 0;                                          /* . and .. */
    char name[13], path[VFS_PATH_MAX];
    short_name(e, name);
    size_t dl = strlen(dir), nl = strlen(name);
    if (dl + 1 + nl + 1 > sizeof path) return 0;
    memcpy(path, dir, dl); path[dl] = '/'; memcpy(path + dl + 1, name, nl + 1);
    uint16_t first = rd16(e + 26);
    if (e[11] & 0x10) {
        if (depth >= 16 || !valid_cluster(fs, first)) { kprintf("fat: skipping bad dir %s\n", path); return 0; }
        int rc = vfs_mkdir(NULL, path);
        if (rc) { kprintf("fat: mkdir %s: %d\n", path, rc); return 0; }
        int n = mount_dir(fs, first, path, depth + 1);
        if (n < 0) return n;
        *files += n;
        return 0;
    }
    uint32_t size = rd32(e + 28);
    if (size && !valid_cluster(fs, first)) { kprintf("fat: skipping bad file %s\n", path); return 0; }
    struct fatfile *ff = kzalloc(sizeof *ff);
    if (!ff) return -ENOMEM;
    ff->fs = fs; ff->first = first;
    int err = 0;
    struct vnode *v = vfs_create(NULL, path, VN_FILE, &err);
    if (!v) { kfree(ff); kprintf("fat: create %s: %d\n", path, err); return 0; }
    v->priv = ff; v->size = size; v->ops = &fat_file_ops;
    vfs_put(v);
    (*files)++;
    return 0;
}

/* clus 0 = the fixed root directory region. Returns files added or <0. */
static int mount_dir(struct fatfs *fs, uint32_t clus, const char *path, int depth) {
    uint8_t sec[512];
    int files = 0;
    if (clus == 0) {
        for (uint32_t s = 0; s < (fs->root_ents * 32 + 511) / 512; s++) {
            if (read_sector(fs, fs->root_lba + s, sec)) return -EIO;
            for (int i = 0; i < 512; i += 32) {
                int r = add_entry(fs, sec + i, path, depth, &files);
                if (r < 0) return r;
                if (r) return files;
            }
        }
        return files;
    }
    for (uint32_t steps = 0; valid_cluster(fs, clus) && steps <= fs->nclusters; steps++) {
        for (uint32_t s = 0; s < fs->spc; s++) {
            if (read_sector(fs, cluster_lba(fs, clus) + s, sec)) return -EIO;
            for (int i = 0; i < 512; i += 32) {
                int r = add_entry(fs, sec + i, path, depth, &files);
                if (r < 0) return r;
                if (r) return files;
            }
        }
        clus = fs->fat[clus];
    }
    return files;
}

int fat_find_disk(void) {
    uint8_t *b = kmalloc(512);
    if (!b) return -ENOMEM;
    int found = -ENODEV;
    for (int d = 0; d < ahci_disk_count() && found < 0; d++) {
        struct fatfs tmp;
        if (ahci_read(d, 0, 1, b) == 0 && parse_bpb(b, &tmp) == 0) found = d;
    }
    kfree(b);
    return found;
}

int fat_mount(int disk, const char *path) {
    struct fatfs *fs = kzalloc(sizeof *fs);
    if (!fs) return -ENOMEM;
    fs->disk = disk;
    fs->cache_lba = UINT64_MAX;
    uint8_t sec[512];
    int rc = read_sector(fs, 0, sec);
    if (!rc) rc = parse_bpb(sec, fs);
    if (!rc) {
        fs->fat = kmalloc((fs->nclusters + 2) * 2u);
        if (!fs->fat) rc = -ENOMEM;
    }
    for (uint32_t i = 0; !rc && i < fs->nclusters + 2; i += 256) {
        rc = read_sector(fs, fs->fat_lba + i / 256, sec);
        uint32_t n = fs->nclusters + 2 - i < 256 ? fs->nclusters + 2 - i : 256;
        if (!rc) memcpy(fs->fat + i, sec, n * 2);
    }
    if (!rc) { rc = vfs_mkdir(NULL, path); if (rc == -EEXIST) rc = 0; }
    if (!rc) rc = mount_dir(fs, 0, path, 0);
    if (rc < 0) { kfree(fs->fat); kfree(fs); return rc; }
    kprintf("fat: disk %d mounted at %s: FAT16, %u clusters of %u bytes, %d files\n",
            disk, path, fs->nclusters, fs->spc * 512, rc);
    return rc;   /* fs lives as long as the kernel (no unmount) */
}
