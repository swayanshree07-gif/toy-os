/* FAT16 file system on an AHCI disk (read: Day 15, write: Day 16), see vfs.h.
 *
 * Only "superfloppy" volumes (boot sector at LBA 0, no partition table) with 512-byte
 * sectors. At mount the whole FAT is read into memory and the directory tree is copied
 * into the VFS: every directory becomes a ramfs directory and every file a VN_FILE whose
 * ops read the cluster chain from the disk on demand (so file data is never copied).
 * Only 8.3 names (shown in lower case); long-file-name entries, the volume label and
 * deleted entries are skipped. A one-sector cache serves sequential small reads.
 *
 * Writing (Day 16) is write-through: every write/truncate/create/mkdir/unlink updates
 * the data sectors, the directory entry and the changed FAT sectors (in every FAT copy)
 * before it returns, then flushes the disk cache. New clusters are zeroed when they are
 * allocated (so holes read as zeros). New names must be valid 8.3 names. No rename,
 * no timestamps beyond a fixed date, no long names. */
#include "vfs.h"
#include "ahci.h"
#include "heap.h"
#include "string.h"
#include "kernel.h"
#include "kprintf.h"

struct fatfs {
    int disk;
    uint32_t spc, fat_lba, root_lba, root_ents, data_lba, nclusters, nfats, fatsz;
    uint32_t alloc_hint, dirty_lo, dirty_hi;   /* dirty FAT sectors [lo, hi), lo>=hi: none */
    uint16_t *fat;                   /* nclusters + 2 entries */
    uint8_t cache[512];
    uint64_t cache_lba;              /* UINT64_MAX = empty */
};
/* priv of every vnode under the mount point (files and directories). For directories
 * first = 0 means the fixed root region. ent_lba/ent_off locate the 32-byte directory
 * entry (unused for the root). dead: unlinked, the clusters are gone. */
struct fatfile { struct fatfs *fs; uint16_t first; bool dead; uint16_t ent_off; uint64_t ent_lba; };

static struct fat_stats stats;

void fat_get_stats(struct fat_stats *s) { *s = stats; }
static uint32_t count_free(const struct fatfs *fs);

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
    fs->nfats = nfats; fs->fatsz = fatsz; fs->alloc_hint = 2;
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

/* ---- writing ---- */

static int write_sector(struct fatfs *fs, uint64_t lba, const void *buf) {
    int r = ahci_write(fs->disk, lba, 1, buf);
    stats.sector_writes++;
    if (lba == fs->cache_lba) {
        if (r) fs->cache_lba = UINT64_MAX;
        else memcpy(fs->cache, buf, 512);
    }
    return r;
}

static void fat_set(struct fatfs *fs, uint32_t c, uint16_t val) {
    fs->fat[c] = val;
    uint32_t s = c / 256;
    if (fs->dirty_lo >= fs->dirty_hi) { fs->dirty_lo = s; fs->dirty_hi = s + 1; }
    else {
        if (s < fs->dirty_lo) fs->dirty_lo = s;
        if (s + 1 > fs->dirty_hi) fs->dirty_hi = s + 1;
    }
}

/* Write the dirty FAT sectors to every FAT copy, then flush the disk's cache. */
static int fat_sync(struct fatfs *fs) {
    uint8_t sec[512];
    int rc = 0;
    for (uint32_t s = fs->dirty_lo; s < fs->dirty_hi && !rc; s++) {
        uint32_t first = s * 256, n = fs->nclusters + 2 - first;
        if (n > 256) n = 256;
        else if ((rc = read_sector(fs, fs->fat_lba + s, sec))) break;   /* keep the tail */
        memcpy(sec, fs->fat + first, n * 2);
        for (uint32_t k = 0; k < fs->nfats && !rc; k++)
            rc = write_sector(fs, fs->fat_lba + k * fs->fatsz + s, sec);
    }
    fs->dirty_lo = fs->dirty_hi = 0;
    stats.free_clusters = count_free(fs);
    if (!rc && ahci_flush(fs->disk)) rc = -EIO;
    return rc ? -EIO : 0;
}

static uint32_t count_free(const struct fatfs *fs) {
    uint32_t n = 0;
    for (uint32_t c = 2; c < fs->nclusters + 2; c++) n += fs->fat[c] == 0;
    return n;
}

/* Allocate one zeroed cluster marked end-of-chain; 0 if the disk is full. */
static uint32_t alloc_cluster(struct fatfs *fs) {
    static const uint8_t zero[512];
    for (uint32_t i = 0; i < fs->nclusters; i++) {
        uint32_t c = 2 + (fs->alloc_hint - 2 + i) % fs->nclusters;
        if (fs->fat[c]) continue;
        for (uint32_t s = 0; s < fs->spc; s++)
            if (write_sector(fs, cluster_lba(fs, c) + s, zero)) return 0;
        fat_set(fs, c, 0xFFFF);
        fs->alloc_hint = c + 1 < fs->nclusters + 2 ? c + 1 : 2;
        return c;
    }
    return 0;
}

static void free_chain(struct fatfs *fs, uint32_t c) {
    for (uint32_t steps = 0; valid_cluster(fs, c) && steps <= fs->nclusters; steps++) {
        uint32_t next = fs->fat[c];
        fat_set(fs, c, 0);
        c = next;
    }
}

/* Rewrite first cluster + size in the file's directory entry. */
static int update_entry(struct fatfile *ff, uint32_t size) {
    uint8_t sec[512];
    if (read_sector(ff->fs, ff->ent_lba, sec)) return -EIO;
    uint8_t *e = sec + ff->ent_off;
    e[26] = (uint8_t)ff->first; e[27] = (uint8_t)(ff->first >> 8);
    e[28] = (uint8_t)size; e[29] = (uint8_t)(size >> 8); e[30] = (uint8_t)(size >> 16); e[31] = (uint8_t)(size >> 24);
    return write_sector(ff->fs, ff->ent_lba, sec) ? -EIO : 0;
}

/* Make sure the chain has at least `need` clusters. */
static int ensure_clusters(struct fatfile *ff, uint64_t need) {
    struct fatfs *fs = ff->fs;
    uint32_t prev = 0, c = ff->first;
    for (uint64_t i = 0; i < need; i++) {
        if (!valid_cluster(fs, c)) {
            uint32_t nc = alloc_cluster(fs);
            if (!nc) return -ENOSPC;
            if (prev) fat_set(fs, prev, (uint16_t)nc);
            else ff->first = (uint16_t)nc;
            c = nc;
        }
        prev = c;
        c = fs->fat[c];
    }
    return 0;
}

static long write_at(struct fatfile *ff, const void *buf, size_t len, uint64_t off) {
    struct fatfs *fs = ff->fs;
    uint32_t csize = fs->spc * 512;
    int rc = ensure_clusters(ff, (off + len + csize - 1) / csize);
    if (rc) return rc;
    uint32_t c = ff->first;
    for (uint64_t skip = off / csize; skip && valid_cluster(fs, c); skip--) c = fs->fat[c];
    uint8_t sec[512];
    size_t done = 0;
    uint64_t pos = off;
    while (done < len) {
        if (!valid_cluster(fs, c)) return -EIO;
        uint32_t in = (uint32_t)(pos % csize);
        uint64_t lba = cluster_lba(fs, c) + in / 512;
        size_t n = 512 - in % 512;
        if (n > len - done) n = len - done;
        if (n < 512 && read_sector(fs, lba, sec)) return -EIO;     /* partial: read-modify-write */
        memcpy(sec + in % 512, (const uint8_t *)buf + done, n);
        if (write_sector(fs, lba, sec)) return -EIO;
        done += n; pos += n;
        if (pos % csize == 0) c = fs->fat[c];
    }
    return (long)done;
}

static long fat_write(struct vnode *v, const void *buf, size_t len, uint64_t off) {
    struct fatfile *ff = v->priv;
    if (ff->dead) return -EIO;
    if (!len) return 0;
    if (off + len > 0xFFFFFFFFull) return -ENOSPC;
    static const uint8_t zero[512];
    long r = 0;
    for (uint64_t p = v->size; p < off && r >= 0; p += 512)      /* fill a hole with zeros */
        r = write_at(ff, zero, off - p < 512 ? (size_t)(off - p) : 512, p);
    if (r >= 0) r = write_at(ff, buf, len, off);
    if (r > 0 && off + (uint64_t)r > v->size) v->size = off + (uint64_t)r;
    int rc = update_entry(ff, (uint32_t)v->size);
    int rc2 = fat_sync(ff->fs);
    if (r >= 0 && (rc || rc2)) r = -EIO;
    return r;
}

static int fat_truncate(struct vnode *v) {
    struct fatfile *ff = v->priv;
    if (ff->dead) return -EIO;
    free_chain(ff->fs, ff->first);
    ff->first = 0;
    v->size = 0;
    int rc = update_entry(ff, 0);
    int rc2 = fat_sync(ff->fs);
    return rc ? rc : rc2;
}

/* "notes.txt" -> "NOTES   TXT"; -ENAMETOOLONG unless it is a valid 8.3 name. */
static int name83(const char *name, uint8_t out[11]) {
    memset(out, ' ', 11);
    int i = 0, n = 0;
    bool ext = false;
    if (!name[0] || name[0] == '.') return -ENAMETOOLONG;
    for (; name[i]; i++) {
        char ch = name[i];
        if (ch == '.') {
            if (ext || !name[i + 1]) return -ENAMETOOLONG;
            ext = true; n = 0;
            continue;
        }
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
        for (const char *q = "_-~!#$%&'(){}^@"; *q && !ok; q++) ok = *q == ch;
        if (!ok || (ext ? n >= 3 : n >= 8)) return -ENAMETOOLONG;
        out[(ext ? 8 : 0) + n++] = (uint8_t)ch;
    }
    return 0;
}

#define FAT_DATE ((uint16_t)(((2026 - 1980) << 9) | (11 << 5) | 9))

static void make_entry(uint8_t *e, const uint8_t name[11], uint8_t attr, uint16_t first) {
    memset(e, 0, 32);
    memcpy(e, name, 11);
    e[11] = attr;
    e[16] = e[18] = e[24] = FAT_DATE & 0xFF;
    e[17] = e[19] = e[25] = FAT_DATE >> 8;
    e[26] = (uint8_t)first; e[27] = (uint8_t)(first >> 8);
}

/* Find a free slot in directory `dir` (extending a subdirectory if it is full) and write
 * entry e there; returns its location. */
static int dir_add(struct fatfile *dir, const uint8_t *e, uint64_t *lba_out, uint16_t *off_out) {
    struct fatfs *fs = dir->fs;
    uint8_t sec[512];
    uint32_t c = dir->first, last = 0;
    uint32_t nsec = dir->first ? fs->spc : (fs->root_ents * 32 + 511) / 512;
    for (uint32_t steps = 0; dir->first == 0 ? steps == 0 : valid_cluster(fs, c) && steps <= fs->nclusters; steps++) {
        uint64_t base = dir->first ? cluster_lba(fs, c) : fs->root_lba;
        for (uint32_t s = 0; s < nsec; s++) {
            if (read_sector(fs, base + s, sec)) return -EIO;
            for (int i = 0; i < 512; i += 32) {
                if (sec[i] != 0 && sec[i] != 0xE5) continue;
                memcpy(sec + i, e, 32);
                if (write_sector(fs, base + s, sec)) return -EIO;
                *lba_out = base + s; *off_out = (uint16_t)i;
                return 0;
            }
        }
        if (!dir->first) return -ENOSPC;                   /* fixed root region is full */
        last = c;
        c = fs->fat[c];
    }
    uint32_t nc = alloc_cluster(fs);                       /* zeroed */
    if (!nc) return -ENOSPC;
    fat_set(fs, last, (uint16_t)nc);
    memset(sec, 0, 512);
    memcpy(sec, e, 32);
    if (write_sector(fs, cluster_lba(fs, nc), sec)) return -EIO;
    *lba_out = cluster_lba(fs, nc); *off_out = 0;
    return 0;
}

static const struct vnode_ops fat_file_ops, fat_dir_ops;

static int fat_create(struct vnode *dir, struct vnode *child) {
    struct fatfile *dn = dir->priv;
    struct fatfs *fs = dn->fs;
    if (dn->dead) return -ENOENT;
    if (child->type != VN_FILE && child->type != VN_DIR) return -EPERM;
    uint8_t name[11], e[32];
    int rc = name83(child->name, name);
    if (rc) return rc;
    struct fatfile *ff = kzalloc(sizeof *ff);
    if (!ff) return -ENOMEM;
    ff->fs = fs;
    if (child->type == VN_DIR) {
        uint32_t c = alloc_cluster(fs);
        if (!c) { kfree(ff); fat_sync(fs); return -ENOSPC; }
        uint8_t sec[512] = {0};
        static const uint8_t dot[11] = ".          ", dotdot[11] = "..         ";
        make_entry(sec, dot, 0x10, (uint16_t)c);
        make_entry(sec + 32, dotdot, 0x10, dn->first);
        if (write_sector(fs, cluster_lba(fs, c), sec)) rc = -EIO;
        ff->first = (uint16_t)c;
    }
    make_entry(e, name, child->type == VN_DIR ? 0x10 : 0x20, ff->first);
    if (!rc) rc = dir_add(dn, e, &ff->ent_lba, &ff->ent_off);
    if (rc) free_chain(fs, ff->first);
    int rc2 = fat_sync(fs);
    if (!rc) rc = rc2;
    if (rc) { kfree(ff); return rc; }
    child->priv = ff;
    child->ops = child->type == VN_DIR ? &fat_dir_ops : &fat_file_ops;
    return 0;
}

static int fat_unlink(struct vnode *dir, struct vnode *child) {
    (void)dir;
    struct fatfile *ff = child->priv;
    if (!ff || ff->dead) return -EIO;
    uint8_t sec[512];
    if (read_sector(ff->fs, ff->ent_lba, sec)) return -EIO;
    sec[ff->ent_off] = 0xE5;
    if (write_sector(ff->fs, ff->ent_lba, sec)) return -EIO;
    free_chain(ff->fs, ff->first);
    ff->dead = true;
    return fat_sync(ff->fs);
}

static const struct vnode_ops fat_file_ops = {
    .read = fat_read, .write = fat_write, .truncate = fat_truncate, .release = fat_release,
};
static const struct vnode_ops fat_dir_ops = {
    .create = fat_create, .unlink = fat_unlink, .release = fat_release,
};

/* Give the (already populated) VFS directory at path its disk identity. */
static int attach_dir(struct fatfs *fs, const char *path, uint32_t first, uint64_t lba, int off) {
    struct fatfile *ff = kzalloc(sizeof *ff);
    if (!ff) return -ENOMEM;
    ff->fs = fs; ff->first = (uint16_t)first; ff->ent_lba = lba; ff->ent_off = (uint16_t)off;
    int err = 0;
    struct vnode *v = vfs_lookup(NULL, path, &err);
    if (!v) { kfree(ff); return err; }
    v->priv = ff;
    v->ops = &fat_dir_ops;
    vfs_put(v);
    return 0;
}

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
static int attach_dir(struct fatfs *fs, const char *path, uint32_t first, uint64_t lba, int off);

/* Handle one 32-byte entry. 1 = end of directory, 0 = continue, <0 error. */
static int add_entry(struct fatfs *fs, const uint8_t *e, const char *dir, int depth, int *files,
                     uint64_t lba, int off) {
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
        return attach_dir(fs, path, first, lba, off);
    }
    uint32_t size = rd32(e + 28);
    if (size && !valid_cluster(fs, first)) { kprintf("fat: skipping bad file %s\n", path); return 0; }
    struct fatfile *ff = kzalloc(sizeof *ff);
    if (!ff) return -ENOMEM;
    ff->fs = fs; ff->first = first; ff->ent_lba = lba; ff->ent_off = (uint16_t)off;
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
                int r = add_entry(fs, sec + i, path, depth, &files, fs->root_lba + s, i);
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
                int r = add_entry(fs, sec + i, path, depth, &files, cluster_lba(fs, clus) + s, i);
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
    if (rc >= 0) { int a = attach_dir(fs, path, 0, 0, 0); if (a) rc = a; }
    if (rc >= 0) stats.free_clusters = count_free(fs);
    if (rc < 0) { kfree(fs->fat); kfree(fs); return rc; }
    kprintf("fat: disk %d mounted at %s: FAT16, %u clusters of %u bytes, %d files\n",
            disk, path, fs->nclusters, fs->spc * 512, rc);
    return rc;   /* fs lives as long as the kernel (no unmount) */
}
