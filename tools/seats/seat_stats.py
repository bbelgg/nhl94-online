#!/usr/bin/env python3
"""seat_stats.py -- per-seat movement table from NHL94_SEATS_TRACE lines.

    python3 tools/seats/seat_stats.py <stderr log> [--expect-joy-all]

For every seat, period and held direction: traced frames, mean vx / vy,
x / y travelled while the direction was held, and the facing at the end.
Checks on every traced frame: the pfjoycon mask (joy=) is exactly the seats'
objects (no stray human, no human lost), and each seat's pad player word
(c=) is its object (no pad switch). --expect-joy-all: also require all 12
objects human (joy=FFF) on every frame where every seat has a player.
"""
import re
import sys

LINE = re.compile(r'nhl94_seats: t=(\d+) per=(\d+) .* joy=([0-9A-F]+) ')
SEAT = re.compile(r's(\d+) (\w+:\w+) (?:sc=(-?\d+) c=(-?\d+) pad=([0-9A-F]+) x=(-?\d+) y=(-?\d+) '
                  r'vx=(-?\d+) vy=(-?\d+) f=(\d+)|none)')
DIRS = {1: 'UP', 2: 'DOWN', 4: 'LEFT', 8: 'RIGHT'}


def main():
    path = sys.argv[1]
    want_all = '--expect-joy-all' in sys.argv
    groups = {}       # (seat, per, dir) -> list of (x, y, vx, vy, f)
    names = {}
    frames = joy_bad = c_bad = none_frames = all_bad = 0
    for line in open(path, errors='replace'):
        m = LINE.search(line)
        if not m:
            continue
        frames += 1
        per = int(m.group(2))
        joy = int(m.group(3), 16)
        objs = set()
        seats = SEAT.findall(line.split(' | ', 1)[1] if ' | ' in line else '')
        any_none = False
        for s in seats:
            k = int(s[0])
            names[k] = s[1]
            if not s[2]:
                any_none = True
                continue
            sc, c, pad = int(s[2]), int(s[3]), int(s[4], 16)
            objs.add(sc)
            if c != sc:
                c_bad += 1
            d = DIRS.get(pad & 0xF)
            if d:
                groups.setdefault((k, per, d), []).append(tuple(int(v) for v in s[5:10]))
        if any_none:
            none_frames += 1
        if joy != sum(1 << o for o in objs):
            joy_bad += 1
            if joy_bad <= 5:
                print('joy mismatch: t=%s joy=%03X seats=%s' % (m.group(1), joy, sorted(objs)))
        if want_all and not any_none and joy != 0xFFF:
            all_bad += 1
    print('traced_frames=%d joy_mask_mismatch=%d pad_player_mismatch=%d frames_with_a_vacant_seat=%d%s'
          % (frames, joy_bad, c_bad, none_frames, ' not_all_12_human=%d' % all_bad if want_all else ''))
    print('%-4s %-9s %-3s %-6s %5s %8s %8s %6s %6s %s' % ('seat', 'role', 'per', 'held', 'n', 'mean_vx',
                                                        'mean_vy', 'dx', 'dy', 'facing'))
    for key in sorted(groups):
        k, per, d = key
        g = groups[key]
        n = len(g)
        print('s%-3d %-9s %-3d %-6s %5d %8d %8d %6d %6d %d->%d' % (
            k, names.get(k, '?'), per, d, n, sum(v[2] for v in g) / n, sum(v[3] for v in g) / n,
            g[-1][0] - g[0][0], g[-1][1] - g[0][1], g[0][4], g[-1][4]))


if __name__ == '__main__':
    main()
