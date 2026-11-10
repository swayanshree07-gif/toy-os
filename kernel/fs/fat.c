/* FAT16 file system on an AHCI disk (read: Day 15, write: Day 16), see vfs.h.
 *
 * Only "superfloppy" volumes (boot sector at LBA 0, no partition table) with 512-byte
 * sectors. At mount the whole FAT is read into memory and the directory tree is copied
 * into the VFS: every directory becomes a ramfs directory and every file a VN_FILE whose
 * ops read the cluster chain from the disk on demand (so file data is never copied).
 * 8.3 names are shown in lower case; VFAT long file names (Day 24) are read and written
 * (ASCII/Latin-1 only, up to VFS_NAME_MAX bytes). The volume label and deleted entries are
 * skipped. A one-sector cache serves sequential small reads.
 *
 * Writing (Day 16) is write-through: every write/truncate/create/mkdir/unlink updates
 * the data sectors, the directory entry and the changed FAT sectors (in every FAT copy)
 * before it returns, then flushes the disk cache. New clusters are zeroed when they are
 * allocated (so holes read as zeros). A name that is not a lower-case 8.3 name gets LFN
 * entries plus a generated short alias (NAME~N.EXT). rename() within one volume moves
 * the entries (Day 24). No timestamps beyond a fixed date. */
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
    char mount[24];
};
#define FAT_MAX_MOUNTS 4
static struct fatfs *mounts[FAT_MAX_MOUNTS];
static int nmounts;
/* priv of every vnode under the mount point (files and directories). For directories
 * first = 0 means the fixed root region. ent_lba/ent_off locate the 32-byte directory
 * entry (unused for the root). dead: unlinked, the clusters are gone. */
#define LFN_MAX 5                /* ceil(VFS_NAME_MAX / 13) */
struct fatfile {
    struct fatfs *fs; uint16_t first; bool dead; uint16_t ent_off; uint64_t ent_lba;
    uint8_t nlfn;                    /* LFN slots in front of the short entry */
    uint16_t lfn_off[LFN_MAX]; uint64_t lfn_lba[LFN_MAX];
};

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

/* ---- directory slots and long names (Day 24) ---- */

/* Location of slot idx of directory `first` (0 = root). -ENOENT past the end. */
static int slot_at(struct fatfs *fs, uint32_t first, uint32_t idx, uint64_t *lba, uint16_t *off) {
    if (!first) {
        if (idx >= fs->root_ents) return -ENOENT;
        *lba = fs->root_lba + idx / 16; *off = (uint16_t)(idx % 16 * 32);
        return 0;
    }
    uint32_t per = fs->spc * 16, c = first;
    for (uint32_t k = idx / per; k; k--) {
        if (!valid_cluster(fs, c)) return -ENOENT;
        c = fs->fat[c];
    }
    if (!valid_cluster(fs, c)) return -ENOENT;
    *lba = cluster_lba(fs, c) + idx % per / 16; *off = (uint16_t)(idx % 16 * 32);
    return 0;
}

static int slot_read(struct fatfs *fs, uint64_t lba, uint16_t off, uint8_t e[32]) {
    uint8_t sec[512];
    if (read_sector(fs, lba, sec)) return -EIO;
    memcpy(e, sec + off, 32);
    return 0;
}

static int slot_write(struct fatfs *fs, uint64_t lba, uint16_t off, const uint8_t e[32]) {
    uint8_t sec[512];
    if (read_sector(fs, lba, sec)) return -EIO;
    memcpy(sec + off, e, 32);
    return write_sector(fs, lba, sec) ? -EIO : 0;
}

static uint8_t lfn_sum(const uint8_t raw[11]) {
    uint8_t s = 0;
    for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + raw[i]);
    return s;
}

static const uint8_t lfn_offs[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

static bool is_lower83(const char *name) {
    uint8_t tmp[11];
    if (name83(name, tmp)) return false;
    for (const char *p = name; *p; p++) if (*p >= 'A' && *p <= 'Z') return false;
    return true;
}

/* Is the 8.3 name raw used by a live entry in directory `first`? */
static int short_used(struct fatfs *fs, uint32_t first, const uint8_t raw[11]) {
    uint64_t lba; uint16_t off; uint8_t e[32];
    for (uint32_t i = 0; slot_at(fs, first, i, &lba, &off) == 0; i++) {
        if (slot_read(fs, lba, off, e)) return -EIO;
        if (e[0] == 0) return 0;
        if (e[0] != 0xE5 && e[11] != 0x0F && memcmp(e, raw, 11) == 0) return 1;
    }
    return 0;
}

/* Short name for `name` in directory `first`: the name itself if it is 8.3, else
 * BASE~N.EXT built from its valid characters. */
static int make_short(struct fatfs *fs, uint32_t first, const char *name, uint8_t out[11]) {
    if (name83(name, out) == 0) {
        int u = short_used(fs, first, out);
        if (u <= 0) return u;
    }
    const char *dot = NULL;
    for (const char *p = name + 1; *p; p++) if (*p == '.') dot = p;
    char base[9], ext[4];
    int nb = 0, nx = 0;
    for (const char *p = name; *p && p != dot; p++) {
        char ch = *p;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        uint8_t t[11]; char one[2] = {ch, 0};
        if (ch != '.' && name83(one, t) == 0 && nb < 8) base[nb++] = ch;
    }
    for (const char *p = dot ? dot + 1 : ""; *p && nx < 3; p++) {
        char ch = *p;
        if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
        uint8_t t[11]; char one[2] = {ch, 0};
        if (ch != '.' && name83(one, t) == 0) ext[nx++] = ch;
    }
    if (!nb) { memcpy(base, "FILE", 4); nb = 4; }
    for (int n = 1; n < 1000; n++) {
        char tail[5]; int tl = 0;
        tail[tl++] = '~';
        if (n >= 100) tail[tl++] = (char)('0' + n / 100);
        if (n >= 10) tail[tl++] = (char)('0' + n / 10 % 10);
        tail[tl++] = (char)('0' + n % 10);
        int keep = nb < 8 - tl ? nb : 8 - tl;
        memset(out, ' ', 11);
        memcpy(out, base, (size_t)keep);
        memcpy(out + keep, tail, (size_t)tl);
        memcpy(out + 8, ext, (size_t)nx);
        int u = short_used(fs, first, out);
        if (u <= 0) return u;
    }
    return -EEXIST;
}

/* Write `name` (+ its LFN slots if needed) with the fields of e (name bytes replaced)
 * into directory dir; records the slot locations in ff. */
static int dir_add_named(struct fatfile *dir, const char *name, uint8_t e[32], struct fatfile *ff) {
    struct fatfs *fs = dir->fs;
    int rc = make_short(fs, dir->first, name, e);
    if (rc) return rc;
    size_t len = strlen(name);
    int nl = is_lower83(name) ? 0 : (int)((len + 12) / 13);
    if (nl > LFN_MAX) return -ENAMETOOLONG;
    int need = nl + 1;
    uint64_t lba[LFN_MAX + 1]; uint16_t off[LFN_MAX + 1];
    int run = 0;
    uint32_t i = 0;
    uint8_t cur[32];
    for (; run < need && slot_at(fs, dir->first, i, &lba[run], &off[run]) == 0; i++) {
        if (slot_read(fs, lba[run], off[run], cur)) return -EIO;
        if (cur[0] == 0 || cur[0] == 0xE5) run++;
        else run = 0;
    }
    if (run < need) {                       /* extend: whole run in a fresh cluster */
        if (!dir->first) return -ENOSPC;
        uint32_t last = dir->first;
        while (valid_cluster(fs, fs->fat[last])) last = fs->fat[last];
        uint32_t nc = alloc_cluster(fs);    /* zeroed */
        if (!nc) return -ENOSPC;
        fat_set(fs, last, (uint16_t)nc);
        for (int k = 0; k < need; k++) { lba[k] = cluster_lba(fs, nc) + k / 16; off[k] = (uint16_t)(k % 16 * 32); }
    }
    uint8_t sum = lfn_sum(e);
    for (int k = 0; k < nl; k++) {         /* slot k holds part nl-k (last part first) */
        int part = nl - k;
        uint8_t l[32];
        memset(l, 0, 32);
        l[0] = (uint8_t)(part | (k == 0 ? 0x40 : 0));
        l[11] = 0x0F; l[13] = sum;
        for (int j = 0; j < 13; j++) {
            size_t ci = (size_t)(part - 1) * 13 + (size_t)j;
            uint16_t ch = ci < len ? (uint8_t)name[ci] : ci == len ? 0 : 0xFFFF;
            l[lfn_offs[j]] = (uint8_t)ch; l[lfn_offs[j] + 1] = (uint8_t)(ch >> 8);
        }
        if (slot_write(fs, lba[k], off[k], l)) return -EIO;
        ff->lfn_lba[k] = lba[k]; ff->lfn_off[k] = off[k];
    }
    if (slot_write(fs, lba[nl], off[nl], e)) return -EIO;
    ff->nlfn = (uint8_t)nl; ff->ent_lba = lba[nl]; ff->ent_off = off[nl];
    return 0;
}

/* Mark the short entry and its LFN slots deleted. */
static int drop_entries(struct fatfile *ff) {
    uint8_t e[32];
    for (int k = 0; k <= ff->nlfn; k++) {
        uint64_t lba = k < ff->nlfn ? ff->lfn_lba[k] : ff->ent_lba;
        uint16_t off = k < ff->nlfn ? ff->lfn_off[k] : ff->ent_off;
        if (slot_read(ff->fs, lba, off, e)) return -EIO;
        e[0] = 0xE5;
        if (slot_write(ff->fs, lba, off, e)) return -EIO;
    }
    return 0;
}

static const struct vnode_ops fat_file_ops, fat_dir_ops;

static int fat_create(struct vnode *dir, struct vnode *child) {
    struct fatfile *dn = dir->priv;
    struct fatfs *fs = dn->fs;
    if (dn->dead) return -ENOENT;
    if (child->type != VN_FILE && child->type != VN_DIR) return -EPERM;
    uint8_t e[32];
    int rc = 0;
    static const uint8_t blank[11] = "           ";
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
    make_entry(e, blank, child->type == VN_DIR ? 0x10 : 0x20, ff->first);
    if (!rc) rc = dir_add_named(dn, child->name, e, ff);
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
    if (drop_entries(ff)) return -EIO;
    free_chain(ff->fs, ff->first);
    ff->dead = true;
    return fat_sync(ff->fs);
}

/* Move src's entries to directory newdir under `name` (the VFS has already removed an
 * existing target). The new entries are written before the old ones are dropped. */
static int fat_rename(struct vnode *src, struct vnode *newdir, const char *name) {
    struct fatfile *ff = src->priv, *dn = newdir->priv;
    if (!ff || !dn || ff->dead || dn->dead) return -EIO;
    if (ff->fs != dn->fs) return -EXDEV;
    struct fatfs *fs = ff->fs;
    uint8_t e[32];
    if (slot_read(fs, ff->ent_lba, ff->ent_off, e)) return -EIO;
    struct fatfile old = *ff, neu = *ff;
    int rc = dir_add_named(dn, name, e, &neu);
    if (!rc) rc = drop_entries(&old);
    if (!rc && src->type == VN_DIR) {          /* fix ".." of a moved directory */
        uint8_t dd[32];
        rc = slot_read(fs, cluster_lba(fs, ff->first), 32, dd);
        dd[26] = (uint8_t)dn->first; dd[27] = (uint8_t)(dn->first >> 8);
        if (!rc) rc = slot_write(fs, cluster_lba(fs, ff->first), 32, dd);
    }
    if (!rc) *ff = neu;
    int rc2 = fat_sync(fs);
    return rc ? rc : rc2;
}

static const struct vnode_ops fat_file_ops = {
    .read = fat_read, .write = fat_write, .truncate = fat_truncate, .release = fat_release,
    .rename = fat_rename,
};
static const struct vnode_ops fat_dir_ops = {
    .create = fat_create, .unlink = fat_unlink, .release = fat_release, .rename = fat_rename,
};


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

/* Long name being collected while a directory is scanned. */
struct lfn_state {
    int want, n;                     /* next expected sequence number, slots seen */
    uint8_t sum;
    bool bad;                        /* a non-ASCII/Latin-1 character or too long */
    char name[LFN_MAX * 13 + 1];
    uint64_t lba[LFN_MAX]; uint16_t off[LFN_MAX];
};

static void lfn_take(struct lfn_state *ls, const uint8_t *e, uint64_t lba, int off) {
    int seq = e[0] & 0x1F;
    if (e[0] & 0x40) {
        memset(ls, 0, sizeof *ls);
        if (seq < 1 || seq > LFN_MAX) { ls->want = 0; return; }
        ls->want = seq; ls->sum = e[13];
        ls->name[seq * 13] = 0;
    }
    if (!ls->want || seq != ls->want || e[13] != ls->sum) { ls->want = 0; ls->n = 0; return; }
    for (int j = 0; j < 13; j++) {
        uint16_t ch = rd16(e + lfn_offs[j]);
        char *d = &ls->name[(seq - 1) * 13 + j];
        if (ch == 0) { *d = 0; break; }
        if (ch == 0xFFFF) { *d = 0; continue; }
        if (ch > 0xFF || ch == '/') ls->bad = true;
        *d = (char)ch;
    }
    ls->lba[ls->n] = lba; ls->off[ls->n] = (uint16_t)off; ls->n++;
    ls->want--;
    if (!ls->want) ls->want = -1;    /* complete: the short entry must come next */
}

/* Give the (already populated) VFS directory at path its disk identity. */
static int attach_dir(struct fatfs *fs, const char *path, uint32_t first, uint64_t lba, int off,
                      const struct lfn_state *ls, int nlfn) {
    struct fatfile *ff = kzalloc(sizeof *ff);
    if (!ff) return -ENOMEM;
    ff->fs = fs; ff->first = (uint16_t)first; ff->ent_lba = lba; ff->ent_off = (uint16_t)off;
    ff->nlfn = (uint8_t)nlfn;
    for (int k = 0; k < nlfn; k++) { ff->lfn_lba[k] = ls->lba[k]; ff->lfn_off[k] = ls->off[k]; }
    int err = 0;
    struct vnode *v = vfs_lookup(NULL, path, &err);
    if (!v) { kfree(ff); return err; }
    v->priv = ff;
    v->ops = &fat_dir_ops;
    vfs_put(v);
    return 0;
}

/* Handle one 32-byte entry. 1 = end of directory, 0 = continue, <0 error. */
static int add_entry(struct fatfs *fs, const uint8_t *e, const char *dir, int depth, int *files,
                     uint64_t lba, int off, struct lfn_state *ls) {
    if (e[0] == 0) return 1;
    if (e[0] != 0xE5 && e[11] == 0x0F) { lfn_take(ls, e, lba, off); return 0; }
    bool have_lfn = ls->want == -1 && !ls->bad && ls->sum == lfn_sum(e) && ls->name[0] &&
                    strlen(ls->name) <= VFS_NAME_MAX;
    int nlfn = ls->n;
    ls->want = 0;
    if (e[0] == 0xE5 || (e[11] & 0x08)) return 0;                   /* deleted, label */
    if (e[0] == '.') return 0;                                          /* . and .. */
    char name[VFS_NAME_MAX + 1], path[VFS_PATH_MAX];
    if (have_lfn) memcpy(name, ls->name, strlen(ls->name) + 1);
    else { short_name(e, name); nlfn = 0; }
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
        return attach_dir(fs, path, first, lba, off, ls, nlfn);
    }
    uint32_t size = rd32(e + 28);
    if (size && !valid_cluster(fs, first)) { kprintf("fat: skipping bad file %s\n", path); return 0; }
    struct fatfile *ff = kzalloc(sizeof *ff);
    if (!ff) return -ENOMEM;
    ff->fs = fs; ff->first = first; ff->ent_lba = lba; ff->ent_off = (uint16_t)off;
    ff->nlfn = (uint8_t)nlfn;
    for (int k = 0; k < nlfn; k++) { ff->lfn_lba[k] = ls->lba[k]; ff->lfn_off[k] = ls->off[k]; }
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
    struct lfn_state ls = {0};
    if (clus == 0) {
        for (uint32_t s = 0; s < (fs->root_ents * 32 + 511) / 512; s++) {
            if (read_sector(fs, fs->root_lba + s, sec)) return -EIO;
            for (int i = 0; i < 512; i += 32) {
                int r = add_entry(fs, sec + i, path, depth, &files, fs->root_lba + s, i, &ls);
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
                int r = add_entry(fs, sec + i, path, depth, &files, cluster_lba(fs, clus) + s, i, &ls);
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
    if (rc >= 0) { int a = attach_dir(fs, path, 0, 0, 0, NULL, 0); if (a) rc = a; }
    if (rc >= 0) stats.free_clusters = count_free(fs);
    if (rc < 0) { kfree(fs->fat); kfree(fs); return rc; }
    size_t ml = strlen(path) < sizeof fs->mount - 1 ? strlen(path) : sizeof fs->mount - 1;
    memcpy(fs->mount, path, ml);
    if (nmounts < FAT_MAX_MOUNTS) mounts[nmounts++] = fs;
    kprintf("fat: disk %d mounted at %s: FAT16, %u clusters of %u bytes, %d files\n",
            disk, path, fs->nclusters, fs->spc * 512, rc);
    return rc;   /* fs lives as long as the kernel (no unmount) */
}

bool fat_statfs_locked(struct vnode *v, struct tern_statfs *st) {
    if (v->ops != &fat_file_ops && v->ops != &fat_dir_ops) return false;
    struct fatfs *fs = ((struct fatfile *)v->priv)->fs;
    memcpy(st->fstype, "fat16", 6);
    memcpy(st->mount, fs->mount, sizeof st->mount);
    st->bsize = fs->spc * 512u;
    st->blocks = fs->nclusters;
    st->bfree = count_free(fs);
    st->files = (uint64_t)fs->disk;
    return true;
}

int fat_sync_all_locked(void) {
    for (int i = 0; i < nmounts; i++)
        if (fat_sync(mounts[i])) return -EIO;
    return nmounts;
}

const char *fat_mount_of_disk(int disk) {
    for (int i = 0; i < nmounts; i++)
        if (mounts[i]->disk == disk) return mounts[i]->mount;
    return NULL;
}
