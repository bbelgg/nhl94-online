#!/usr/bin/env python3
"""montage.py out.png cols scale in1.png in2.png ... -- tile PNGs (nearest zoom)."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pngzoom
out, cols, sc = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
ims = []
for p in sys.argv[4:]:
    w, h, bpp, rows = pngzoom.read_png(p)
    z = []
    for r in rows:
        line = b''.join(bytes(r[x * bpp:x * bpp + 3]) * sc for x in range(w))
        z.extend([line] * sc)
    ims.append((w * sc, h * sc, z))
cw = max(i[0] for i in ims); ch = max(i[1] for i in ims); gap = 4
nrows = (len(ims) + cols - 1) // cols
W = cols * cw + (cols + 1) * gap; H = nrows * ch + (nrows + 1) * gap
bg = bytes([24, 24, 28])
canvas = [bytearray(bg * W) for _ in range(H)]
for k, (w, h, z) in enumerate(ims):
    x0 = gap + (k % cols) * (cw + gap); y0 = gap + (k // cols) * (ch + gap)
    for y in range(h):
        canvas[y0 + y][x0 * 3:(x0 + w) * 3] = z[y]
pngzoom.write_png(out, W, H, [bytes(r) for r in canvas])
