#!/usr/bin/env python3
"""gen_input.py -- seeded pseudo-random input scripts for the lock tests (lc_auto, pen_sub, pull_g, ps_4v4).

  gen_input.py <boot .inc/.input file> <players> <frames> <seed> [--check] [--periods N] [--optline V] [--perlen V]

Appends to the boot menu script: every 60-140 frames three random players change direction and sometimes
tap B (pass / call for pass), A (shoot) and, with --check, C (check: draws penalties). Fewer than 4096
directives (input_script.c limit). The output is committed (*.input) so the tests are reproducible."""
import random, sys

def main():
    a = sys.argv[1:]
    boot, n, frames, seed = a[0], int(a[1]), int(a[2]), int(a[3])
    check = '--check' in a
    r = random.Random(seed)
    bt = open(boot).read().rstrip('\n')
    ws = [a[i + 1] for i, x in enumerate(a) if x == '--w']   # ADDR=VAL (16-bit), written next to the period-length write
    if '--optline' in a: ws.append('0xFFD058=%s' % a[a.index('--optline') + 1])   # menu default is OptLine 1 (off): 0 = On
    if ws:
        ins = ''.join('\nWRITE_RAM16 %s 0x%04X' % (w.split('=')[0], int(w.split('=')[1], 0)) for w in ws)
        bt = ''.join(l + (ins if l.startswith('WRITE_RAM16 0xFFD050') else '') + '\n' for l in bt.split('\n')).rstrip('\n')
    if '--perlen' in a:      # 0 = 5 min, 1 = 10, 2 = 20, 3 = 30 s
        v = int(a[a.index('--perlen') + 1])
        bt = ''.join((('WRITE_RAM16 0xFFD050 0x%04X' % v) if l.startswith('WRITE_RAM16 0xFFD050') else l) + '\n'
                     for l in bt.split('\n')).rstrip('\n')
        if 'WRITE_RAM16 0xFFD050' not in bt:
            bt += '\nWRITE_RAM16 0xFFD050 0x%04X' % v
    out = [bt, 'WAIT 300']
    dirs = ['UP', 'DOWN', 'LEFT', 'RIGHT']
    periods = int(a[a.index('--periods') + 1]) if '--periods' in a else 0
    gm = ['0x0000', '0x0200', '0x0000', '0x0200']   # gmode while the clock runs: home attacks up / down / up
    t = 0
    pend = 0
    while t < frames:
        if periods and pend < periods and t >= (pend + 1) * (frames // periods):
            # end of a 30 s period: wait for clock 0, START on the intermission menu, wait for the next period
            out += ['WAIT_RAM16 0xFFC468 0x0000', 'WAIT 330', 'PRESS START 5', 'WAIT_RAM16 0xFFC2EA ' + gm[(pend + 1) % 4], 'WAIT 30']
            pend += 1
        for _ in range(3):
            p = r.randint(1, n)
            out += ['PLAYER %d' % p, 'RELEASE', 'HOLD ' + r.choice(dirs)]
            x = r.random()
            if x < 0.15: out.append('PRESS B 4')
            elif x < 0.25: out.append('PRESS A 4')
            elif check and x < 0.45: out.append('PRESS C 4')
        w = r.randint(60, 140)
        out.append('WAIT %d' % w)
        t += w
    out.append('WAIT 600')
    print('\n'.join(out))
main()
