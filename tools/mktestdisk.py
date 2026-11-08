#!/usr/bin/env python3
"""Write the raw test disk used by `make test` (AHCI driver check).
64 sectors of 512 bytes. Sector n starts with "TERNDISK LBA %08d\n" and the rest is the
byte pattern (n * 7 + i) & 0xFF, so the kernel can verify any sector it reads."""
import sys
SECTORS = 64
out = bytearray()
for n in range(SECTORS):
    s = bytearray((n * 7 + i) & 0xFF for i in range(512))
    head = b"TERNDISK LBA %08d\n" % n
    s[:len(head)] = head
    out += s
with open(sys.argv[1], "wb") as f:
    f.write(out)
