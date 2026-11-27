#!/usr/bin/env python3
"""check_builds.py - verify the per-player builds against the ROM (docs/BUILDS.md).

  check_builds.py <stderr log of a run with NHL94_BUILDS_TRACE=1> [--rom game/NHL94.bin]

For every seat the latch line ("plays as ... attr=<8 ROM bytes>") gives the build's record; every trace line
("obj=.. side=.. pnum=.. pos=.. hc=.. fields=<+$67..+$76>") of an object whose (side, position) is a seat's gets
the fields setplayer SHOULD have computed from that record (collide94.asm setplayer / AttributeCalc):

  AC(n)  = clamp(5*n + trunc(hotcold/3), 0, 30)            hotcold = the on-ice slot's tmHotCold byte
  weight = (b1 & F0) >> 1        legstr = AC(b1&F)         legspd = AC(b2>>4)
  aioff  = fold(clamp(AC(b2&F) + bonuses))                 aidef  = fold(clamp(AC(b3>>4) + homeaway))
  shotspd= AC(b3&F)   checking = clamp(AC(b4>>4) + 3rd)    handed = (b4&1)^1   fighting = b4 & E
  stickhand = clamp(AC(b5>>4)+pp+pk+ha)   shotacc = clamp(AC(b5&F)+pp+pk+ha)
  endurance = AC(b6>>4)   spodds = clamp(AC(b6&F) + 2*3rd)   passacc = clamp(AC(b7>>4)+pp+ha)
  aggress = AC(b7&F) & F   rostnum = b0;   fold(c) = (((c>>1)^F)+F)>>1
Skaters get the team's home/away bonus (scouting report byte 2: home = high nibble, away = -(low nibble));
goalies none. Only first-period lines (gsp == 0) are checked: no 3rd-period bonus, and the faceoffs checked
are 5-on-5 (a power-play bonus would show as a mismatch, reported with the bonus it implies).
Exit 1 on any mismatch, or when nothing was checked.
"""
import re, struct, sys

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    rom_path = "game/NHL94.bin"
    if "--rom" in sys.argv:
        rom_path = sys.argv[sys.argv.index("--rom") + 1]
        args = [a for a in args if a != rom_path]
    rom = open(rom_path, "rb").read()
    u16 = lambda a: struct.unpack(">H", rom[a:a + 2])[0]
    u32 = lambda a: struct.unpack(">I", rom[a:a + 4])[0]
    roles = ["G", "LD", "RD", "LW", "C", "RW"]
    seats = {}      # (side, role) -> (name, attr)
    ok = bad = 0
    ha_cache = {}
    def homeaway(team, side):
        if team not in ha_cache:
            base = u32(0x30E + 4 * team)
            ha_cache[team] = rom[base + u16(base + 8) + 2]
        b = ha_cache[team]
        return (b >> 4) if side == 0 else -(b & 0xF)
    def clamp(c):
        return 0 if c < 0 else 0x1E if c > 0x1E else c
    for line in open(args[0], errors="replace"):
        m = re.search(r"nhl94_builds: t=(\d+) seat (\d+) (home|away):(\w+) plays as (.*?) \(roster slot .*attr=([0-9A-F]{16})", line)
        if m:
            seats[(0 if m.group(3) == "home" else 1, roles.index(m.group(4)))] = (m.group(5), bytes.fromhex(m.group(6)))
            continue
        m = re.search(r"nhl94_builds: t=(\d+) obj=(\d+) side=(\d) team=(\d+) pnum=(\d+) pos=(-?\d+) jersey=(\w+) hc=(-?\d+) gsp=(\d+) fields=([0-9A-F]{32})", line)
        if not m:
            continue
        t, obj, side, team, pnum, pos, jersey, hc, gsp = [int(x) if i != 6 else x for i, x in enumerate(m.groups()[:9])]
        got = bytes.fromhex(m.group(10))
        if pnum >= 26 or pos < 0 or pos > 5 or gsp != 0 or (side, pos) not in seats:
            continue
        name, a = seats[(side, pos)]
        AC = lambda n: clamp(5 * n + int(hc / 3))      # trunc toward zero like divs.w
        ha = 0 if pos == 0 else homeaway(team, side)
        fold = lambda c: (((c >> 1) ^ 0xF) + 0xF) >> 1
        exp = bytes([
            (a[1] & 0xF0) >> 1, AC(a[1] & 0xF), AC(a[2] >> 4),
            fold(clamp(AC(a[2] & 0xF) + ha)), fold(clamp(AC(a[3] >> 4) + ha)), AC(a[3] & 0xF),
            clamp(AC(a[5] & 0xF) + ha), clamp(AC(a[7] >> 4) + ha), a[0],
            clamp(AC(a[6] & 0xF)), clamp(AC(a[5] >> 4) + ha), AC(a[6] >> 4), AC(a[7] & 0xF) & 0xF,
            a[4] & 0xE, clamp(AC(a[4] >> 4)), (a[4] & 1) ^ 1])
        if pos == 0:
            exp = bytes(exp[:3]) + bytes([fold(clamp(AC(a[2] & 0xF))), fold(clamp(AC(a[3] >> 4)))]) + exp[5:]
        if exp == got:
            ok += 1
            print("OK   t=%d obj=%d %s:%s pnum=%d as %s" % (t, obj, "away" if side else "home", roles[pos], pnum, name))
        else:
            bad += 1
            print("DIFF t=%d obj=%d %s:%s pnum=%d as %s hc=%d ha=%d\n   got %s\n   exp %s" %
                  (t, obj, "away" if side else "home", roles[pos], pnum, name, hc, ha, got.hex(), exp.hex()))
    print("check_builds: %d objects match, %d differ" % (ok, bad))
    sys.exit(1 if bad or not ok else 0)

if __name__ == "__main__":
    main()
