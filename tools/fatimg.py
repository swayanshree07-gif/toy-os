#!/usr/bin/env python3
"""fatimg: TernOS's own FAT16 disk image tool (no mtools needed).

Superfloppy FAT16 images (no partition table, 512-byte sectors, 8.3 names), the same
layout the kernel's /disk driver (kernel/fs/fat.c) mounts.

  fatimg.py mkfs  IMG [SECTORS] [LABEL]   new empty FAT16 image (default 8192 sectors)
  fatimg.py ls    IMG [PATH] [-R]         list a directory ("d name" / "f name size")
  fatimg.py put   IMG HOSTFILE PATH       copy a host file in (replaces an existing file)
  fatimg.py get   IMG PATH [HOSTFILE]     copy a file out (to stdout if no HOSTFILE)
  fatimg.py mkdir IMG PATH                create a directory (parent must exist)
  fatimg.py rm    IMG PATH                remove a file or an empty directory
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
        (self.raw, self.attr, _, _, _, _, _, _, _, _, self.clus,
         self.size) = struct.unpack_from("<11sBBBHHHHHHHI", img, where)

    @property
    def is_dir(self):
        return bool(self.attr & 0x10)

    @property
    def name(self):
        return pretty(self.raw)


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
        for off in self.dir_slots(clus):
            b = self.img[off]
            if b == 0:
                break
            if b == 0xE5:
                continue
            e = Entry(self.img, off)
            if e.attr == 0x0F or e.attr & 0x08 or e.raw[:1] == b".":
                continue           # LFN, volume label, . and ..
            out.append(e)
        return out

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
        raw = name83(name)
        for e in self.entries(clus):
            if e.raw == raw:
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
        raw = name83(parts[-1])
        e = self.find(parent, parts[-1])
        if e is not None:
            if e.is_dir:
                raise FatError("is a directory: %s" % path)
            if e.clus:
                self.free_chain(e.clus)
            off = e.where
        else:
            off = self.new_slot(parent)
        n = (len(data) + self.csize - 1) // self.csize
        cl = self.alloc(n)
        for k, c in enumerate(cl):
            chunk = data[k * self.csize:(k + 1) * self.csize]
            self.img[self.coff(c):self.coff(c) + len(chunk)] = chunk
        self.write_entry(off, raw, 0x20, cl[0] if cl else 0, len(data))

    def mkdir(self, path):
        parts = self.split(path)
        if not parts:
            raise FatError("bad path")
        parent = self.lookup_dir(parts[:-1])
        if self.find(parent, parts[-1]) is not None:
            raise FatError("exists: %s" % path)
        raw = name83(parts[-1])
        off = self.new_slot(parent)
        c = self.alloc(1)[0]
        self.write_entry(self.coff(c), name83("."), 0x10, c, 0)
        self.write_entry(self.coff(c) + 32, name83(".."), 0x10, parent, 0)
        self.write_entry(off, raw, 0x10, c, 0)

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
        self.img[e.where] = 0xE5

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
