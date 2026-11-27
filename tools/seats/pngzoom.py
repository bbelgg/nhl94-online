#!/usr/bin/env python3
"""pngzoom.py in.png out.png x y w h scale -- crop + nearest-neighbour zoom
(stdlib only; 8-bit RGB/RGBA non-interlaced PNGs as the runner writes them).
Use scale 1 and the full size for a plain copy; x y w h = 0 0 0 0 = whole image."""
import struct, sys, zlib

def read_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, w = 8, b'', None
    while pos < len(d):
        n, t = struct.unpack('>I4s', d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        pos += 12 + n
        if t == b'IHDR':
            w, h, bd, ct, _, _, il = struct.unpack('>IIBBBBB', body)
            assert bd == 8 and ct in (2, 6) and il == 0, (bd, ct, il)
        elif t == b'IDAT':
            idat += body
    bpp = 3 if ct == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        cur = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: cur[i] = (cur[i] + a) & 255
            elif f == 2: cur[i] = (cur[i] + b) & 255
            elif f == 3: cur[i] = (cur[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 255
        rows.append(bytes(cur)); prev = cur
    return w, h, bpp, rows

def write_png(path, w, h, rows):
    raw = b''.join(b'\x00' + r for r in rows)
    def chunk(t, b):
        c = struct.pack('>I', len(b)) + t + b
        return c + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    x, y, cw, ch, s = (int(v) for v in sys.argv[3:8])
    w, h, bpp, rows = read_png(src)
    if cw == 0: x, y, cw, ch = 0, 0, w, h
    out = []
    for yy in range(y, min(y + ch, h)):
        r = rows[yy]
        line = b''.join(bytes(r[xx * bpp:xx * bpp + 3]) * s for xx in range(x, min(x + cw, w)))
        out.extend([line] * s)
    write_png(dst, len(out[0]) // 3, len(out), out)
