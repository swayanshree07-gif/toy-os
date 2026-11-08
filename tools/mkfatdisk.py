#!/usr/bin/env python3
"""Write the FAT16 test disk used by `make test` (own writer, no mkfs/mtools).
Superfloppy layout (no partition table): 8192 sectors of 512 bytes, 1 sector per
cluster, 2 FATs, 512 root entries. 8.3 names only. Contents:
  /HELLO.TXT              "Hello from a FAT16 disk!\n"
  /DOCS/README.TXT        a few lines of text
  /DOCS/DEEP/NEST.TXT     "nested ok\n"
  /BIG.BIN                100000 bytes, byte i = (i * 31 + i // 256) & 0xFF, with its
                          clusters deliberately scattered (reverse order with gaps)
  /EMPTY.TXT              0 bytes (first cluster 0)
The kernel test (kernel/fs/fat.c test) checks these exact contents."""
import struct, sys

SECTORS, SPC, NFATS, ROOT_ENTS, RESERVED = 8192, 1, 2, 512, 1
FATSZ = 32                      # sectors per FAT: 8192 entries * 2 bytes / 512
ROOT_SECS = ROOT_ENTS * 32 // 512
DATA0 = RESERVED + NFATS * FATSZ + ROOT_SECS
NCLUST = (SECTORS - DATA0) // SPC

img = bytearray(SECTORS * 512)
fat = [0] * (NCLUST + 2)
fat[0], fat[1] = 0xFFF8, 0xFFFF
next_free = [2]

def alloc(n, scatter=False):
    if n == 0:
        return []
    if scatter:  # reverse order with one-cluster gaps, then fill the gaps later
        base = next_free[0]
        cl = [base + 2 * (n - 1 - k) for k in range(n)]
        next_free[0] = base + 2 * n
    else:
        cl = list(range(next_free[0], next_free[0] + n))
        next_free[0] += n
    for a, b in zip(cl, cl[1:] + [None]):
        fat[a] = b if b else 0xFFFF
    return cl

def write_chain(cl, data):
    for k, c in enumerate(cl):
        off = (DATA0 + (c - 2) * SPC) * 512
        chunk = data[k * 512 * SPC:(k + 1) * 512 * SPC]
        img[off:off + len(chunk)] = chunk

def name83(name):
    base, _, ext = name.partition(".")
    return base.upper().ljust(8).encode() + ext.upper().ljust(3).encode()

def dirent(name, attr, clus, size):
    raw = name if isinstance(name, bytes) else name83(name)
    return struct.pack("<11sBBBHHHHHHHI", raw, attr, 0, 0, 0, 0, 0, 0, 0x5000, 0x5568,
                       clus, size)

def make_file(data, scatter=False):
    cl = alloc((len(data) + 511) // 512, scatter)
    write_chain(cl, data)
    return (cl[0] if cl else 0), len(data)

def make_dir(entries, parent_clus):
    """entries: list of (name, attr, clus, size); returns first cluster."""
    n = (len(entries) + 2) * 32
    cl = alloc((n + 511) // 512)
    body = dirent(b".          ", 0x10, cl[0], 0) + dirent(b"..         ", 0x10, parent_clus, 0)
    body += b"".join(dirent(*e) for e in entries)
    write_chain(cl, body)
    return cl[0]

big = bytes((i * 31 + i // 256) & 0xFF for i in range(100000))
readme = b"TernOS FAT16 test disk.\nThis file lives in /disk/docs.\nThird line.\n"

hello = make_file(b"Hello from a FAT16 disk!\n")
bigf = make_file(big, scatter=True)
filler = make_file(b"x" * 512 * 3)    # an ordinary file after the scattered one
nest = make_file(b"nested ok\n")
# directory clusters: DOCS needs its own cluster before DEEP knows its parent
docs_cl = alloc(1)[0]
deep = make_dir([("NEST.TXT", 0x20, nest[0], nest[1])], docs_cl)
readme_f = make_file(readme)
docs_body = dirent(b".          ", 0x10, docs_cl, 0) + dirent(b"..         ", 0x10, 0, 0)
docs_body += dirent("README.TXT", 0x20, *readme_f) + dirent("DEEP", 0x10, deep, 0)
write_chain([docs_cl], docs_body)

root = [dirent(b"TERNDISK   ", 0x08, 0, 0),          # volume label: must be skipped
        dirent("HELLO.TXT", 0x20, *hello),
        dirent(b"\xe5ELETED TXT", 0x20, 0, 0),        # deleted entry: must be skipped
        dirent("DOCS", 0x10, docs_cl, 0),
        dirent("BIG.BIN", 0x20, *bigf),
        dirent("EMPTY.TXT", 0x20, 0, 0),
        dirent("FILLER.DAT", 0x20, *filler)]
roff = (RESERVED + NFATS * FATSZ) * 512
img[roff:roff + 32 * len(root)] = b"".join(root)

for k in range(NFATS):
    off = (RESERVED + k * FATSZ) * 512
    raw = struct.pack("<%dH" % len(fat), *fat)
    img[off:off + len(raw)] = raw

bpb = struct.pack("<3s8sHBHBHHBHHHII", b"\xeb\x3c\x90", b"TERNOS  ", 512, SPC, RESERVED,
                  NFATS, ROOT_ENTS, SECTORS, 0xF8, FATSZ, 32, 64, 0, 0)
bpb += struct.pack("<BBBI11s8s", 0x80, 0, 0x29, 0x7E57D15C, b"TERNDISK   ", b"FAT16   ")
img[0:len(bpb)] = bpb
img[510:512] = b"\x55\xaa"
with open(sys.argv[1], "wb") as f:
    f.write(img)
