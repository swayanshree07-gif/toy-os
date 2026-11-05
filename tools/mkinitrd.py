#!/usr/bin/env python3
"""Pack a directory into a TernOS initrd image ("TRD" format, see kernel/include/initrd.h).

Layout (all integers little-endian):
  header  16 bytes: magic b"TERNRD1\\0", u32 file count, u32 total image size
  entries 64 bytes each: name[48] (NUL-padded path relative to the root, '/'-separated),
          u32 data offset (from image start, 16-aligned), u32 size, u32 crc32, u32 flags (0)
  data    each file's bytes, padded to 16
Files are sorted by path so the image is reproducible.
usage: mkinitrd.py <dir> <out.img> [path/in/initrd=host/file ...]
"""
import os, struct, sys, zlib

MAGIC = b"TERNRD1\0"
NAME_MAX = 48

def main():
    root, out = sys.argv[1], sys.argv[2]
    files = []
    for d, dirs, names in os.walk(root):
        dirs.sort()
        for n in sorted(names):
            p = os.path.join(d, n)
            rel = os.path.relpath(p, root).replace(os.sep, "/")
            if len(rel.encode()) >= NAME_MAX:
                sys.exit(f"mkinitrd: name too long: {rel}")
            with open(p, "rb") as f:
                files.append((rel, f.read()))
    for extra in sys.argv[3:]:
        rel, src = extra.split("=", 1)
        if len(rel.encode()) >= NAME_MAX:
            sys.exit(f"mkinitrd: name too long: {rel}")
        with open(src, "rb") as f:
            files = [x for x in files if x[0] != rel] + [(rel, f.read())]
    files.sort()
    off = 16 + 64 * len(files)
    off = (off + 15) & ~15
    entries, blobs = b"", b""
    for rel, data in files:
        entries += struct.pack("<48sIIII", rel.encode(), off + len(blobs), len(data),
                               zlib.crc32(data) & 0xFFFFFFFF, 0)
        blobs += data + b"\0" * (-len(data) % 16)
    head = entries + b"\0" * (off - 16 - len(entries))
    total = 16 + len(head) + len(blobs)
    img = MAGIC + struct.pack("<II", len(files), total) + head + blobs
    assert len(img) == total
    with open(out, "wb") as f:
        f.write(img)

if __name__ == "__main__":
    main()
