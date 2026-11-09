#!/usr/bin/env python3
"""fatimg: TernOS's own FAT16 disk image tool (no mtools needed).

Superfloppy FAT16 images (no partition table, 512-byte sectors, 8.3 names plus VFAT
long file names since Day 24), the same
layout the kernel's /disk driver (kernel/fs/fat.c) mounts.

  fatimg.py mkfs  IMG [SECTORS] [LABEL]   new empty FAT16 image (default 8192 sectors)
  fatimg.py ls    IMG [PATH] [-R]         list a directory ("d name" / "f name size")
  fatimg.py put   IMG HOSTFILE PATH       copy a host file in (replaces an existing file)
  fatimg.py get   IMG PATH [HOSTFILE]     copy a file out (to stdout if no HOSTFILE)
  fatimg.py mkdir IMG PATH                create a directory (parent must exist)
  fatimg.py rm    IMG PATH                remove a file or an empty directory
  fatimg.py mv    IMG FROM TO             rename/move (TO's parent must exist, TO must not)
  fatimg.py fsck  IMG                     consistency check; exit 1 on any problem

Paths use "/" and are case-insensitive ("/docs/readme.txt"). Exit status 0 = ok,
1 = error (message on stderr).
"""
import struct
import sys

SECTOR = 512
EOC = 0xFFF8          # entries >= this end a chain
BAD = 0xFFF7
DATE, TIME = 0x5000 | (1 << 5) | 9, 0x6000   # 2020-01-09 12:00, fixed for reproducibility


class FatError(Exception):
    pass


def name83(name):
    """'readme.txt' -> b'README  TXT'; FatError if it isn't a valid 8.3 name."""
    if name in (".", ".."):
        return name.encode().ljust(11)
    base, dot, ext = name.partition(".")
    ok = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~!#$%&'(){}^@")
    up_b, up_e = base.upper(), ext.upper()
    if (not base or len(base) > 8 or len(ext) > 3 or "." in ext or (dot and not ext)
            or not set(up_b + up_e) <= ok):
        raise FatError("not a valid 8.3 name: %r" % name)
    return up_b.ljust(8).encode() + up_e.ljust(3).encode()


LFN_OFFS = (1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30)   # 13 UCS-2 chars per entry


def needs_lfn(name):
    """Long name needed unless the name is a valid 8.3 name written in lower case."""
    try:
        name83(name)
    except FatError:
        return True
    return name != name.lower()


def lfn_checksum(raw):
    s = 0
    for b in raw:
        s = (((s & 1) << 7) + (s >> 1) + b) & 0xFF
    return s


def lfn_entries(name, raw):
    """LFN slots for `name`, in on-disk order (last part first)."""
    u = name.encode("utf-16-le")
    chars = [u[i:i + 2] for i in range(0, len(u), 2)]
    parts = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    ck = lfn_checksum(raw)
    out = []
    for k, part in enumerate(parts):
        part = part + ([b"\0\0"] if len(part) < 13 else [])
        part += [b"\xff\xff"] * (13 - len(part))
        e = bytearray(32)
        e[0] = (k + 1) | (0x40 if k == len(parts) - 1 else 0)
        e[11], e[13] = 0x0F, ck
        for o, c in zip(LFN_OFFS, part):
            e[o:o + 2] = c
        out.append(bytes(e))
    return out[::-1]


def pretty(raw):
    base = raw[:8].decode("latin-1").rstrip()
    ext = raw[8:11].decode("latin-1").rstrip()
    if base and base[0] == "\x05":
        base = "\xe5" + base[1:]
    return (base + ("." + ext if ext else "")).lower()


class Entry:
    """One 32-byte directory entry; `where` = byte offset of it in the image."""
    def __init__(self, img, where):
        self.where = where
        self.lname, self.lfn = None, []     # long name, offsets of its LFN slots
        (self.raw, self.attr, _, _, _, _, _, _, _, _, self.clus,
         self.size) = struct.unpack_from("<11sBBBHHHHHHHI", img, where)

    @property
    def is_dir(self):
        return bool(self.attr & 0x10)

    @property
    def name(self):
        return self.lname or pretty(self.raw)


class Image:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.img = bytearray(f.read())
        d = self.img
        if len(d) < SECTOR or d[510:512] != b"\x55\xaa":
            raise FatError("no boot signature")
        (bps, self.spc, self.rsv, self.nfats, self.rootents, tot16, _media, self.fatsz,
         _, _, _, tot32) = struct.unpack_from("<HBHBHHBHHHII", d, 11)
        if bps != SECTOR or self.spc == 0 or self.nfats == 0 or self.fatsz == 0:
            raise FatError("unsupported BPB")
        self.total = tot16 or tot32
        self.rootsec = self.rsv + self.nfats * self.fatsz
        self.rootsecs = (self.rootents * 32 + SECTOR - 1) // SECTOR
        self.data0 = self.rootsec + self.rootsecs
        self.nclust = (self.total - self.data0) // self.spc
        if not 4085 <= self.nclust < 65525:
            raise FatError("not FAT16 (%d clusters)" % self.nclust)
        self.csize = self.spc * SECTOR
        off = self.rsv * SECTOR
        self.fat = list(struct.unpack_from("<%dH" % (self.nclust + 2), d, off))

    # ---- low level -------------------------------------------------------------------
    def save(self):
        raw = struct.pack("<%dH" % len(self.fat), *self.fat)
        for k in range(self.nfats):
            off = (self.rsv + k * self.fatsz) * SECTOR
            self.img[off:off + len(raw)] = raw
        with open(self.path, "wb") as f:
            f.write(self.img)

    def coff(self, c):
        return (self.data0 + (c - 2) * self.spc) * SECTOR

    def chain(self, first):
        out, c, seen = [], first, set()
        while 2 <= c < EOC:
            if c >= self.nclust + 2 or c == BAD or c in seen:
                raise FatError("broken cluster chain at %d" % c)
            seen.add(c)
            out.append(c)
            c = self.fat[c]
        return out

    def alloc(self, n):
        free = [c for c in range(2, self.nclust + 2) if self.fat[c] == 0][:n]
        if len(free) < n:
            raise FatError("disk full")
        for a, b in zip(free, free[1:] + [None]):
            self.fat[a] = b if b else 0xFFFF
        for c in free:
            off = self.coff(c)
            self.img[off:off + self.csize] = bytes(self.csize)
        return free

    def free_chain(self, first):
        for c in self.chain(first):
            self.fat[c] = 0

    def dir_slots(self, clus):
        """Byte offsets of every entry slot in a directory (clus 0 = root)."""
        if clus == 0:
            base = self.rootsec * SECTOR
            return [base + 32 * i for i in range(self.rootents)]
        return [self.coff(c) + 32 * i for c in self.chain(clus)
                for i in range(self.csize // 32)]

    def entries(self, clus):
        out = []
        lfn, lslots, lck, want = {}, [], None, 0
        for off in self.dir_slots(clus):
            b = self.img[off]
            if b == 0:
                break
            if b == 0xE5:
                lfn, lslots, want = {}, [], 0
                continue
            e = Entry(self.img, off)
            if e.attr == 0x0F:
                seq = b & 0x1F
                if b & 0x40:
                    lfn, lslots, lck, want = {}, [], self.img[off + 13], seq
                if seq != want or seq == 0 or self.img[off + 13] != lck:
                    lfn, lslots, want = {}, [], 0
                    continue
                lfn[seq] = b"".join(bytes(self.img[off + o:off + o + 2]) for o in LFN_OFFS)
                lslots.append(off)
                want -= 1
                continue
            if not (e.attr & 0x08 or e.raw[:1] == b".") and lfn and want == 0 \
                    and lck == lfn_checksum(e.raw):
                u = b"".join(lfn[k] for k in sorted(lfn))
                n = u.decode("utf-16-le", "replace").split("\0")[0]
                e.lname, e.lfn = n, lslots
            lfn, lslots, want = {}, [], 0
            if e.attr & 0x08 or e.raw[:1] == b".":
                continue           # volume label, . and ..
            out.append(e)
        return out

    def short_for(self, clus, name):
        """8.3 raw name for `name` in directory clus (NAME~N.EXT if a long name is needed)."""
        if not needs_lfn(name):
            return name83(name)
        ok = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~!#$%&'(){}^@")
        base, dot, ext = name.rpartition(".")
        if not dot or not base:
            base, ext = name, ""
        clean = lambda t: "".join(c for c in t.upper() if c in ok)
        b, x = clean(base) or "FILE", clean(ext)[:3]
        try:
            if name83(name) and not (len(b) > 8):
                cand = name83(name)
                if all(e.raw != cand for e in self.entries(clus)):
                    return cand        # valid 8.3 but mixed case: keep the plain short name
        except FatError:
            pass
        used = {e.raw for e in self.entries(clus)}
        for n in range(1, 1000):
            t = "~%d" % n
            raw = (b[:8 - len(t)] + t).ljust(8).encode() + x.ljust(3).encode()
            if raw not in used:
                return raw
        raise FatError("no free short name for %r" % name)

    def free_run(self, clus, n):
        """n consecutive free slots (extends a subdirectory if needed)."""
        slots = self.dir_slots(clus)
        run = []
        for off in slots:
            if self.img[off] in (0, 0xE5):
                run.append(off)
                if len(run) == n:
                    return run
            else:
                run = []
        if clus == 0:
            raise FatError("root directory full")
        last = self.chain(clus)[-1]
        c = self.alloc(1)[0]
        self.fat[last] = c
        return [self.coff(c) + 32 * i for i in range(n)]

    def add_named(self, clus, name, attr, first, size):
        raw = self.short_for(clus, name)
        lf = lfn_entries(name, raw) if needs_lfn(name) else []
        run = self.free_run(clus, len(lf) + 1)
        for off, ent in zip(run, lf):
            self.img[off:off + 32] = ent
        self.write_entry(run[-1], raw, attr, first, size)
        return run[-1]

    def drop(self, e):
        for off in e.lfn + [e.where]:
            self.img[off] = 0xE5

    # ---- paths -----------------------------------------------------------------------
    @staticmethod
    def split(path):
        return [p for p in path.split("/") if p]

    def lookup_dir(self, parts):
        clus = 0
        for p in parts:
            e = self.find(clus, p)
            if e is None or not e.is_dir:
                raise FatError("no such directory: %s" % "/".join(parts))
            clus = e.clus
        return clus

    def find(self, clus, name):
        for e in self.entries(clus):
            if e.name.lower() == name.lower() or pretty(e.raw) == name.lower():
                return e
        return None

    def new_slot(self, clus):
        slots = self.dir_slots(clus)
        for off in slots:
            if self.img[off] in (0, 0xE5):
                return off
        if clus == 0:
            raise FatError("root directory full")
        last = self.chain(clus)[-1]
        c = self.alloc(1)[0]
        self.fat[last] = c
        return self.coff(c)

    def write_entry(self, off, raw, attr, clus, size):
        self.img[off:off + 32] = struct.pack("<11sBBBHHHHHHHI", raw, attr, 0, 0, TIME,
                                             DATE, DATE, 0, TIME, DATE, clus, size)

    # ---- commands --------------------------------------------------------------------
    def ls(self, path, recursive=False, prefix=""):
        clus = self.lookup_dir(self.split(path))
        lines = []
        for e in self.entries(clus):
            if e.is_dir:
                lines.append("d %s%s" % (prefix, e.name))
                if recursive:
                    sub = path.rstrip("/") + "/" + e.name
                    lines += self.ls(sub, True, prefix + e.name + "/")
            else:
                lines.append("f %s%s %d" % (prefix, e.name, e.size))
        return lines

    def get(self, path):
        parts = self.split(path)
        if not parts:
            raise FatError("is a directory: /")
        e = self.find(self.lookup_dir(parts[:-1]), parts[-1])
        if e is None or e.is_dir:
            raise FatError("no such file: %s" % path)
        data = b"".join(bytes(self.img[self.coff(c):self.coff(c) + self.csize])
                        for c in self.chain(e.clus))
        if len(data) < e.size:
            raise FatError("chain shorter than file size: %s" % path)
        return data[:e.size]

    def put(self, path, data):
        parts = self.split(path)
        if not parts:
            raise FatError("bad path")
        parent = self.lookup_dir(parts[:-1])
        if len(parts[-1]) > 55:
            raise FatError("name too long: %s" % parts[-1])
        e = self.find(parent, parts[-1])
        if e is not None:
            if e.is_dir:
                raise FatError("is a directory: %s" % path)
            if e.clus:
                self.free_chain(e.clus)
            self.drop(e)
        n = (len(data) + self.csize - 1) // self.csize
        cl = self.alloc(n)
        for k, c in enumerate(cl):
            chunk = data[k * self.csize:(k + 1) * self.csize]
            self.img[self.coff(c):self.coff(c) + len(chunk)] = chunk
        self.add_named(parent, parts[-1], 0x20, cl[0] if cl else 0, len(data))

    def mkdir(self, path):
        parts = self.split(path)
        if not parts:
            raise FatError("bad path")
        parent = self.lookup_dir(parts[:-1])
        if self.find(parent, parts[-1]) is not None:
            raise FatError("exists: %s" % path)
        if len(parts[-1]) > 55:
            raise FatError("name too long: %s" % parts[-1])
        c = self.alloc(1)[0]
        self.write_entry(self.coff(c), name83("."), 0x10, c, 0)
        self.write_entry(self.coff(c) + 32, name83(".."), 0x10, parent, 0)
        self.add_named(parent, parts[-1], 0x10, c, 0)

    def rm(self, path):
        parts = self.split(path)
        if not parts:
            raise FatError("cannot remove /")
        e = self.find(self.lookup_dir(parts[:-1]), parts[-1])
        if e is None:
            raise FatError("no such file or directory: %s" % path)
        if e.is_dir and self.entries(e.clus):
            raise FatError("directory not empty: %s" % path)
        if e.clus:
            self.free_chain(e.clus)
        self.drop(e)

    def mv(self, src, dst):
        sp, dp = self.split(src), self.split(dst)
        if not sp or not dp:
            raise FatError("bad path")
        e = self.find(self.lookup_dir(sp[:-1]), sp[-1])
        if e is None:
            raise FatError("no such file or directory: %s" % src)
        parent = self.lookup_dir(dp[:-1])
        if self.find(parent, dp[-1]) is not None:
            raise FatError("exists: %s" % dst)
        if e.is_dir:
            c, seen = parent, 0
            while c and seen < 64:          # not into its own subtree
                if c == e.clus:
                    raise FatError("can't move a directory into itself")
                c = Entry(self.img, self.coff(c) + 32).clus
                seen += 1
            self.write_entry(self.coff(e.clus) + 32, name83(".."), 0x10, parent, 0)
        attr, clus, size = e.attr, e.clus, e.size
        self.drop(e)
        self.add_named(parent, dp[-1], attr, clus, size)

    def fsck(self):
        """Returns a list of problems (empty = consistent)."""
        probs = []
        d = self.img
        fats = [d[(self.rsv + k * self.fatsz) * SECTOR:(self.rsv + (k + 1) * self.fatsz) * SECTOR]
                for k in range(self.nfats)]
        if any(f != fats[0] for f in fats[1:]):
            probs.append("FAT copies differ")
        if self.fat[0] & 0xFF != d[21]:
            probs.append("FAT[0] does not match the media byte")
        owner = {}

        def walk(clus, path, parent, depth):
            if depth > 32:
                probs.append("%s: too deep" % path)
                return
            if clus:
                try:
                    slots = self.dir_slots(clus)
                except FatError as ex:
                    probs.append("%s: %s" % (path, ex))
                    return
                dot, dotdot = Entry(d, slots[0]), Entry(d, slots[1])
                if dot.raw != name83(".") or dot.clus != clus:
                    probs.append("%s: bad '.' entry" % path)
                if dotdot.raw != name83("..") or dotdot.clus != parent:
                    probs.append("%s: bad '..' entry" % path)
            for e in self.entries(clus):
                p = path.rstrip("/") + "/" + e.name
                try:
                    cl = self.chain(e.clus) if e.clus else []
                except FatError as ex:
                    probs.append("%s: %s" % (p, ex))
                    continue
                for c in cl:
                    if c in owner:
                        probs.append("%s: cluster %d cross-linked with %s" % (p, c, owner[c]))
                    owner[c] = p
                if e.is_dir:
                    if not cl:
                        probs.append("%s: directory without clusters" % p)
                    else:
                        walk(e.clus, p, clus, depth + 1)
                elif len(cl) != (e.size + self.csize - 1) // self.csize:
                    probs.append("%s: size %d but %d clusters" % (p, e.size, len(cl)))
        walk(0, "/", 0, 0)
        lost = [c for c in range(2, self.nclust + 2) if self.fat[c] not in (0, BAD)
                and c not in owner]
        if lost:
            probs.append("%d lost clusters (first %d)" % (len(lost), lost[0]))
        return probs

    def free_clusters(self):
        return sum(1 for c in range(2, self.nclust + 2) if self.fat[c] == 0)


def mkfs(path, sectors=8192, label="TERNOS"):
    spc = 1
    while (sectors // spc) > 65524:
        spc *= 2
    rsv, nfats, rootents = 1, 2, 512
    rootsecs = rootents * 32 // SECTOR
    fatsz = 1
    while True:      # smallest FAT that covers every cluster
        nclust = (sectors - rsv - nfats * fatsz - rootsecs) // spc
        if (nclust + 2) * 2 <= fatsz * SECTOR:
            break
        fatsz += 1
    if not 4085 <= nclust < 65525:
        raise FatError("%d sectors gives %d clusters: not a FAT16 size" % (sectors, nclust))
    img = bytearray(sectors * SECTOR)
    lab = label.upper().encode()[:11].ljust(11)
    bpb = struct.pack("<3s8sHBHBHHBHHHII", b"\xeb\x3c\x90", b"TERNOS  ", SECTOR, spc, rsv,
                      nfats, rootents, sectors if sectors < 65536 else 0, 0xF8, fatsz,
                      32, 64, 0, sectors if sectors >= 65536 else 0)
    bpb += struct.pack("<BBBI11s8s", 0x80, 0, 0x29, 0x7E57F00D, lab, b"FAT16   ")
    img[0:len(bpb)] = bpb
    img[510:512] = b"\x55\xaa"
    for k in range(nfats):
        off = (rsv + k * fatsz) * SECTOR
        img[off:off + 4] = struct.pack("<HH", 0xFFF8, 0xFFFF)
    root = (rsv + nfats * fatsz) * SECTOR
    img[root:root + 32] = struct.pack("<11sB20x", lab, 0x08)    # volume label entry
    with open(path, "wb") as f:
        f.write(img)


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 1
    cmd, path, args = argv[1], argv[2], argv[3:]
    try:
        if cmd == "mkfs":
            mkfs(path, int(args[0]) if args else 8192, args[1] if len(args) > 1 else "TERNOS")
            return 0
        im = Image(path)
        if cmd == "ls":
            rec = "-R" in args
            rest = [a for a in args if a != "-R"]
            for line in im.ls(rest[0] if rest else "/", rec):
                print(line)
        elif cmd == "get":
            data = im.get(args[0])
            if len(args) > 1:
                with open(args[1], "wb") as f:
                    f.write(data)
            else:
                sys.stdout.buffer.write(data)
        elif cmd == "put":
            with open(args[0], "rb") as f:
                im.put(args[1], f.read())
            im.save()
        elif cmd == "mkdir":
            im.mkdir(args[0])
            im.save()
        elif cmd == "rm":
            im.rm(args[0])
            im.save()
        elif cmd == "mv":
            im.mv(args[0], args[1])
            im.save()
        elif cmd == "fsck":
            probs = im.fsck()
            for p in probs:
                print("fsck: " + p)
            if probs:
                return 1
            print("fsck: ok, %d of %d clusters free" % (im.free_clusters(), im.nclust))
        else:
            sys.stderr.write("fatimg: unknown command %s\n" % cmd)
            return 1
    except (FatError, IndexError, OSError) as ex:
        sys.stderr.write("fatimg: %s\n" % (ex if not isinstance(ex, IndexError)
                                           else "missing argument"))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
