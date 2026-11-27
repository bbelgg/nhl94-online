#!/usr/bin/env python3
"""tagprobe.py img.png... -- with NHL94_NAMETAGS_DEBUG=1 (magenta projection
cross at each tagged skater's origin), print the cross centre and the bounding
box of the game's own star outline (navy) around it: if the tag projection and
the sprites lag each other, the offset changes while the skater moves."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pngzoom
for path in sys.argv[1:]:
    w, h, bpp, rows = pngzoom.read_png(path)
    mag, navy = [], []
    for y in range(h):
        for x in range(w):
            r, g, b = rows[y][x * bpp:x * bpp + 3]
            if (r, g, b) == (255, 0, 255): mag.append((x, y))
            elif r < 40 and g < 40 and 90 < b < 160: navy.append((x, y))
    if not mag: print(path, 'no cross'); continue
    mx = sum(p[0] for p in mag) / len(mag); my = sum(p[1] for p in mag) / len(mag)
    nv = [p for p in navy if abs(p[0] - mx) < 22 and abs(p[1] - my) < 22]
    if not nv: print(path, 'cross', mx, my, 'no star'); continue
    x0, x1 = min(p[0] for p in nv), max(p[0] for p in nv)
    y0, y1 = min(p[1] for p in nv), max(p[1] for p in nv)
    print('%s cross=(%.1f,%.1f) star bbox x %d..%d y %d..%d centre=(%.1f,%.1f) cross-star=(%.1f,%.1f)' % (
        os.path.basename(path), mx, my, x0, x1, y0, y1, (x0 + x1) / 2, (y0 + y1) / 2,
        mx - (x0 + x1) / 2, my - (y0 + y1) / 2))
