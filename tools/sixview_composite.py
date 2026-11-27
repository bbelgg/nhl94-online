#!/usr/bin/env python3
"""sixview_composite.py -- labelled grid PNGs + MP4 from tick-aligned screenshots.

Two input layouts (auto-detected, or forced with --mode):

  procs   one PNG per process per tick, as the engine writes them with
          GENESIS_SCREENSHOT_AT_EXIT=<dir>/<name>.png + GENESIS_SCREENSHOT_AT_TICK:
              <dir>/<name>.png.t<tick>.png
          Tile order and labels come from <dir>/tiles.tsv ("name<TAB>label", one
          per line, in grid order; tools/sixview_localhost.sh writes it), or from
          --tiles name=LABEL,..., else every <name> found, sorted.
          Optional <dir>/shots.tsv ("name tick live confirmed", whitespace
          separated) colours each tile's border: green = the frame showed the
          confirmed state, orange = a prediction (live != confirmed), grey = no
          confirmed digest to compare with.

  views   one process rendering several views itself (GENESIS_VIEWS=6,
          GENESIS_VIEW_SCREENSHOT_EVERY=n, GENESIS_VIEW_SCREENSHOT_DIR=<dir>):
              <dir>/view<k>_t<tick>.png      (k = 0..5; main_t<tick>.png ignored)
          Labels from --labels "L0,L1,..." or <dir>/views.tsv ("k<TAB>label"),
          else "VIEW k".

Output (in --out): grid_t<tick 6 digits>.png per tick, sixview.mp4 (unless
--no-mp4), index.tsv (tick, tiles present). Frames of different sizes (the
Genesis switches between H32 256-px and H40 320-px modes) are scaled with
nearest-neighbour to one tile size (default 640x448, 2x). A tile missing at a tick
is drawn as a grey "MISSING" box.

Pure python3 stdlib (zlib PNG reader/writer, built-in 5x7 bitmap font: this
machine's ffmpeg has no drawtext). ffmpeg (libx264, else mpeg4) only encodes
the MP4. A PNG this reader cannot parse (interlaced, 16-bit) is converted with
ffmpeg to raw RGB as a fallback.
"""
import argparse
import glob
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

# ----------------------------------------------------------------------------- font
# 5x7 glyphs: 7 rows of 5 columns ('#' = on).
_FONT_ROWS = {
    'A': ".###. #...# #...# ##### #...# #...# #...#",
    'B': "####. #...# #...# ####. #...# #...# ####.",
    'C': ".###. #...# #.... #.... #.... #...# .###.",
    'D': "####. #...# #...# #...# #...# #...# ####.",
    'E': "##### #.... #.... ####. #.... #.... #####",
    'F': "##### #.... #.... ####. #.... #.... #....",
    'G': ".###. #...# #.... #.### #...# #...# .####",
    'H': "#...# #...# #...# ##### #...# #...# #...#",
    'I': ".###. ..#.. ..#.. ..#.. ..#.. ..#.. .###.",
    'J': "..### ...#. ...#. ...#. ...#. #..#. .##..",
    'K': "#...# #..#. #.#.. ##... #.#.. #..#. #...#",
    'L': "#.... #.... #.... #.... #.... #.... #####",
    'M': "#...# ##.## #.#.# #.#.# #...# #...# #...#",
    'N': "#...# #...# ##..# #.#.# #..## #...# #...#",
    'O': ".###. #...# #...# #...# #...# #...# .###.",
    'P': "####. #...# #...# ####. #.... #.... #....",
    'Q': ".###. #...# #...# #...# #.#.# #..#. .##.#",
    'R': "####. #...# #...# ####. #.#.. #..#. #...#",
    'S': ".#### #.... #.... .###. ....# ....# ####.",
    'T': "##### ..#.. ..#.. ..#.. ..#.. ..#.. ..#..",
    'U': "#...# #...# #...# #...# #...# #...# .###.",
    'V': "#...# #...# #...# #...# #...# .#.#. ..#..",
    'W': "#...# #...# #...# #.#.# #.#.# #.#.# .#.#.",
    'X': "#...# #...# .#.#. ..#.. .#.#. #...# #...#",
    'Y': "#...# #...# .#.#. ..#.. ..#.. ..#.. ..#..",
    'Z': "##### ....# ...#. ..#.. .#... #.... #####",
    '0': ".###. #...# #..## #.#.# ##..# #...# .###.",
    '1': "..#.. .##.. ..#.. ..#.. ..#.. ..#.. .###.",
    '2': ".###. #...# ....# ...#. ..#.. .#... #####",
    '3': "##### ...#. ..#.. ...#. ....# #...# .###.",
    '4': "...#. ..##. .#.#. #..#. ##### ...#. ...#.",
    '5': "##### #.... ####. ....# ....# #...# .###.",
    '6': "..##. .#... #.... ####. #...# #...# .###.",
    '7': "##### ....# ...#. ..#.. .#... .#... .#...",
    '8': ".###. #...# #...# .###. #...# #...# .###.",
    '9': ".###. #...# #...# .#### ....# ...#. .##..",
    ' ': "..... ..... ..... ..... ..... ..... .....",
    ':': "..... .##.. .##.. ..... .##.. .##.. .....",
    '-': "..... ..... ..... ##### ..... ..... .....",
    '=': "..... ..... ##### ..... ##### ..... .....",
    '_': "..... ..... ..... ..... ..... ..... #####",
    '/': "..... ....# ...#. ..#.. .#... #.... .....",
    '.': "..... ..... ..... ..... ..... .##.. .##..",
    ',': "..... ..... ..... ..... .##.. ..#.. .#...",
    '(': "...#. ..#.. .#... .#... .#... ..#.. ...#.",
    ')': ".#... ..#.. ...#. ...#. ...#. ..#.. .#...",
    '#': ".#.#. .#.#. ##### .#.#. ##### .#.#. .#.#.",
    '!': "..#.. ..#.. ..#.. ..#.. ..#.. ..... ..#..",
    '?': ".###. #...# ....# ...#. ..#.. ..... ..#..",
    '%': "##..# ##.#. ...#. ..#.. .#... .#.## #..##",
    '+': "..... ..#.. ..#.. ##### ..#.. ..#.. .....",
    '>': ".#... ..#.. ...#. ....# ...#. ..#.. .#...",
    '<': "...#. ..#.. .#... #.... .#... ..#.. ...#.",
    '%': "##... ##..# ...#. ..#.. .#... #..## ...##",
    '*': "..... ..#.. #.#.# .###. #.#.# ..#.. .....",
    "'": "..#.. ..#.. .#... ..... ..... ..... .....",
    '@': ".###. #...# #.### #.#.# #.### #.... .###.",
}


def _glyph(ch):
    rows = _FONT_ROWS.get(ch.upper(), _FONT_ROWS['?']).split()
    return [[c == '#' for c in row] for row in rows]


_GLYPHS = {}


def glyph(ch):
    if ch not in _GLYPHS:
        _GLYPHS[ch] = _glyph(ch)
    return _GLYPHS[ch]


# ----------------------------------------------------------------------------- PNG io
def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path):
    """-> (width, height, bytearray RGB). Raises ValueError on unsupported."""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('not a PNG')
    pos, idat, plte = 8, [], None
    w = h = depth = ctype = interlace = None
    while pos < len(data):
        ln, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif typ == b'PLTE':
            plte = body
        elif typ == b'IDAT':
            idat.append(body)
        elif typ == b'IEND':
            break
    if depth != 8 or interlace != 0 or ctype not in (0, 2, 3, 4, 6):
        raise ValueError('unsupported PNG (depth %s type %s interlace %s)' % (depth, ctype, interlace))
    bpp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(b''.join(idat))
    stride = w * bpp
    out = bytearray(stride * h)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                ul = prev[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + _paeth(left, prev[i], ul)) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    if ctype == 2:
        return w, h, out
    rgb = bytearray(w * h * 3)
    if ctype == 6:
        rgb[0::3], rgb[1::3], rgb[2::3] = out[0::4], out[1::4], out[2::4]
    elif ctype == 4:
        rgb[0::3] = rgb[1::3] = rgb[2::3] = out[0::2]
    elif ctype == 0:
        rgb[0::3] = rgb[1::3] = rgb[2::3] = out
    else:  # palette
        for i, ix in enumerate(out):
            rgb[i * 3:i * 3 + 3] = plte[ix * 3:ix * 3 + 3]
    return w, h, rgb


def read_png_ffmpeg(path, ffmpeg):
    probe = subprocess.run([ffmpeg, '-v', 'error', '-i', path, '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                           capture_output=True)
    if probe.returncode != 0:
        raise ValueError(probe.stderr.decode(errors='replace').strip())
    m = re.search(rb'(\d{2,5})x(\d{2,5})', subprocess.run([ffmpeg, '-hide_banner', '-i', path],
                                                          capture_output=True).stderr)
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, bytearray(probe.stdout)


def write_png(path, w, h, rgb):
    stride = w * 3
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xFFFFFFFF)
    with open(path + '.tmp', 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(bytes(raw), 6)))
        f.write(chunk(b'IEND', b''))
    os.replace(path + '.tmp', path)


# ----------------------------------------------------------------------------- canvas
class Canvas:
    def __init__(self, w, h, color=(0, 0, 0)):
        self.w, self.h = w, h
        self.px = bytearray(bytes(color) * (w * h))

    def rect(self, x, y, w, h, color):
        x0, y0 = max(0, x), max(0, y)
        x1, y1 = min(self.w, x + w), min(self.h, y + h)
        if x1 <= x0 or y1 <= y0:
            return
        row = bytes(color) * (x1 - x0)
        for yy in range(y0, y1):
            o = (yy * self.w + x0) * 3
            self.px[o:o + len(row)] = row

    def blit_scaled(self, x, y, tw, th, sw, sh, src):
        """Nearest-neighbour scale src (sw x sh RGB) into tw x th at (x, y)."""
        xmap = [min(sw - 1, (i * sw) // tw) * 3 for i in range(tw)]
        for ty in range(th):
            sy = min(sh - 1, (ty * sh) // th)
            srow = src[sy * sw * 3:(sy + 1) * sw * 3]
            line = bytearray(tw * 3)
            line[0::3] = bytes(srow[i] for i in xmap)
            line[1::3] = bytes(srow[i + 1] for i in xmap)
            line[2::3] = bytes(srow[i + 2] for i in xmap)
            yy = y + ty
            if 0 <= yy < self.h:
                o = (yy * self.w + x) * 3
                self.px[o:o + tw * 3] = line

    def text(self, x, y, s, scale=2, color=(255, 255, 255), bg=(0, 0, 0), pad=3):
        """Draw s with the 5x7 font; returns the box width."""
        cw = 6 * scale
        bw = len(s) * cw - scale + 2 * pad
        bh = 7 * scale + 2 * pad
        if bg is not None:
            self.rect(x, y, bw, bh, bg)
        for n, ch in enumerate(s):
            g = glyph(ch)
            gx = x + pad + n * cw
            for r in range(7):
                for c in range(5):
                    if g[r][c]:
                        self.rect(gx + c * scale, y + pad + r * scale, scale, scale, color)
        return bw

    def border(self, x, y, w, h, t, color):
        self.rect(x, y, w, t, color)
        self.rect(x, y + h - t, w, t, color)
        self.rect(x, y, t, h, color)
        self.rect(x + w - t, y, t, h, color)


# ----------------------------------------------------------------------------- inputs
PROC_RE = re.compile(r'^(?P<name>.+)\.png\.t(?P<tick>\d+)\.png$')
VIEW_RE = re.compile(r'^view(?P<k>\d+)_t(?P<tick>\d+)\.png$')


def scan_procs(d):
    files = {}
    for p in glob.glob(os.path.join(d, '*.png.t*.png')):
        m = PROC_RE.match(os.path.basename(p))
        if m:
            files.setdefault(m.group('name'), {})[int(m.group('tick'))] = p
    return files


def scan_views(d):
    files = {}
    for p in glob.glob(os.path.join(d, 'view*_t*.png')):
        m = VIEW_RE.match(os.path.basename(p))
        if m:
            files.setdefault(int(m.group('k')), {})[int(m.group('tick'))] = p
    return files


def read_tsv(path):
    rows = []
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                line = line.rstrip('\n')
                if line and not line.startswith('#'):
                    rows.append(line.split('\t'))
    return rows


def parse_ticks(spec, available):
    if not spec or spec == 'auto':
        return sorted(available)
    out = []
    for part in spec.split(','):
        if ':' in part:
            a, b, *s = part.split(':')
            out.extend(range(int(a), int(b) + 1, int(s[0]) if s else 60))
        elif part:
            out.append(int(part))
    return sorted(set(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input', help='directory with the screenshots')
    ap.add_argument('--out', help='output directory (default <input>/../composite)')
    ap.add_argument('--mode', choices=['auto', 'procs', 'views'], default='auto')
    ap.add_argument('--tiles', help='procs mode: name=LABEL,... in grid order (overrides tiles.tsv)')
    ap.add_argument('--labels', help='views mode: LABEL0,LABEL1,... (overrides views.tsv)')
    ap.add_argument('--ticks', default='auto', help="'auto' (every tick found) or a,b,from:to:step")
    ap.add_argument('--cols', type=int, default=3)
    ap.add_argument('--rows', type=int, default=2)
    ap.add_argument('--tile-size', default='640x448', help='WxH of one tile (default 640x448 = 2x; 320x224 = 1x)')
    ap.add_argument('--gap', type=int, default=12, help='pixels between and around tiles (default 12)')
    ap.add_argument('--title', default='', help='text prefixed to the per-frame header')
    ap.add_argument('--fps', type=float, default=10.0, help='MP4 frame rate (grid frames per second)')
    ap.add_argument('--no-mp4', action='store_true')
    ap.add_argument('--ffmpeg', default=shutil.which('ffmpeg') or '/opt/homebrew/bin/ffmpeg')
    a = ap.parse_args()

    tw, th = (int(v) for v in a.tile_size.lower().split('x'))
    mode = a.mode
    if mode == 'auto':
        mode = 'views' if glob.glob(os.path.join(a.input, 'view*_t*.png')) else 'procs'

    tiles = []      # (key, label)
    status = {}     # (key, tick) -> 'ok' | 'pred' | 'unk'
    if mode == 'procs':
        files = scan_procs(a.input)
        if a.tiles:
            for item in a.tiles.split(','):
                name, _, label = item.partition('=')
                tiles.append((name, label or name))
        else:
            tiles = [(r[0], r[1] if len(r) > 1 else r[0]) for r in read_tsv(os.path.join(a.input, 'tiles.tsv'))]
        if not tiles:
            tiles = [(n, n) for n in sorted(files)]
        for r in read_tsv(os.path.join(a.input, 'shots.tsv')):
            f = r[0].split() if len(r) == 1 else r
            if len(f) >= 4:
                key = (f[0], int(f[1]))
                status[key] = 'unk' if f[3] in ('-', '') else ('ok' if f[2] == f[3] else 'pred')
    else:
        files = scan_views(a.input)
        labels = {}
        for r in read_tsv(os.path.join(a.input, 'views.tsv')):
            if len(r) > 1:
                labels[int(r[0])] = r[1]
        if a.labels:
            labels = dict(enumerate(a.labels.split(',')))
        tiles = [(k, labels.get(k, 'VIEW %d' % k)) for k in sorted(files)]
        for k in sorted(labels):
            if k not in files:
                tiles.append((k, labels[k]))

    if not tiles:
        sys.exit('sixview_composite: no %s-mode screenshots found in %s' % (mode, a.input))
    if len(tiles) > a.cols * a.rows:
        print('sixview_composite: %d tiles, grid holds %d -- extra tiles dropped' % (len(tiles), a.cols * a.rows),
              file=sys.stderr)
        tiles = tiles[:a.cols * a.rows]
    avail = set()
    for key, _ in tiles:
        avail |= set(files.get(key, {}))
    ticks = parse_ticks(a.ticks, avail)
    if not ticks:
        sys.exit('sixview_composite: no ticks to composite')

    out = a.out or os.path.join(os.path.dirname(os.path.abspath(a.input.rstrip('/'))), 'composite')
    os.makedirs(out, exist_ok=True)
    for old in glob.glob(os.path.join(out, 'grid_t*.png')):
        os.remove(old)

    gap, header = max(4, a.gap), 24   # >= 4: the 2 px status border sits in the gap
    W = a.cols * tw + (a.cols + 1) * gap
    H = header + a.rows * th + (a.rows + 1) * gap
    W += W & 1
    H += H & 1
    border_col = {'ok': (40, 200, 60), 'pred': (255, 150, 0), 'unk': (90, 90, 90)}
    index = []
    for tick in ticks:
        cv = Canvas(W, H, (16, 16, 16))
        present = 0
        for i, (key, label) in enumerate(tiles):
            cx = gap + (i % a.cols) * (tw + gap)
            cy = header + gap + (i // a.cols) * (th + gap)
            path = files.get(key, {}).get(tick)
            if path:
                try:
                    try:
                        sw, sh, px = read_png(path)
                    except ValueError:
                        sw, sh, px = read_png_ffmpeg(path, a.ffmpeg)
                    cv.blit_scaled(cx, cy, tw, th, sw, sh, px)
                    present += 1
                except Exception as e:  # unreadable file: show it, keep going
                    cv.rect(cx, cy, tw, th, (60, 0, 0))
                    cv.text(cx + 8, cy + th // 2 - 10, 'UNREADABLE', 2, (255, 255, 255), None)
                    print('sixview_composite: %s: %s' % (path, e), file=sys.stderr)
            else:
                cv.rect(cx, cy, tw, th, (70, 70, 70))
                cv.text(cx + tw // 2 - 42, cy + th // 2 - 10, 'MISSING', 2, (220, 220, 220), None)
            st = status.get((key, tick))
            if st:
                cv.border(cx - 2, cy - 2, tw + 4, th + 4, 2, border_col[st])
            cv.text(cx + 2, cy + 2, str(label), 2, (255, 255, 0), (0, 0, 0))
        head = '%s t=%d  %d/%d views' % (a.title + '  ' if a.title else '', tick, present, len(tiles))
        cv.text(gap, 2, head, 2, (255, 255, 255), None, pad=2)
        write_png(os.path.join(out, 'grid_t%06d.png' % tick), W, H, cv.px)
        index.append((tick, present))
    with open(os.path.join(out, 'index.tsv'), 'w') as f:
        f.write('tick\ttiles_present\ttiles\n')
        for t, n in index:
            f.write('%d\t%d\t%d\n' % (t, n, len(tiles)))
    print('sixview_composite: %d grid(s) %dx%d (%s mode, %d tiles) -> %s' % (len(index), W, H, mode, len(tiles), out))

    if not a.no_mp4:
        mp4 = os.path.join(out, 'sixview.mp4')
        pattern = os.path.join(out, 'grid_t*.png')
        for codec in (['-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-crf', '18'], ['-c:v', 'mpeg4', '-q:v', '3']):
            r = subprocess.run([a.ffmpeg, '-v', 'error', '-y', '-framerate', str(a.fps), '-pattern_type', 'glob',
                                '-i', pattern] + codec + [mp4], capture_output=True)
            if r.returncode == 0:
                print('sixview_composite: %s (%d frames @ %g fps)' % (mp4, len(index), a.fps))
                break
        else:
            print('sixview_composite: ffmpeg failed: %s' % r.stderr.decode(errors='replace'), file=sys.stderr)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
