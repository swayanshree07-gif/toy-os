#!/usr/bin/env python3
"""Summarise a binary PPM (P6) screenshot from QEMU's `screendump`:
prints "<width>x<height> colors=<distinct> top=<rrggbb> top_share=<percent>"."""
import sys
from collections import Counter

def main(path):
    data = open(path, 'rb').read()
    fields, pos = [], 0
    while len(fields) < 4:                      # magic, width, height, maxval
        while data[pos:pos + 1].isspace(): pos += 1
        if data[pos:pos + 1] == b'#':
            pos = data.index(b'\n', pos) + 1; continue
        end = pos
        while not data[end:end + 1].isspace(): end += 1
        fields.append(data[pos:end]); pos = end
    pos += 1
    if fields[0] != b'P6' or fields[3] != b'255':
        sys.exit('not an 8-bit P6 ppm')
    w, h = int(fields[1]), int(fields[2])
    px = data[pos:pos + w * h * 3]
    c = Counter(px[i:i + 3] for i in range(0, len(px), 3))
    top, n = c.most_common(1)[0]
    print(f'{w}x{h} colors={len(c)} top={top.hex()} top_share={100 * n // (w * h)}')

if __name__ == '__main__':
    main(sys.argv[1])
