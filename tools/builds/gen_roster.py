#!/usr/bin/env python3
"""gen_roster.py - dump the NHL 94 rosters from the player's own ROM and emit
nhl94_roster_table.c/.h (docs/BUILDS.md). The build runs it (CMakeLists.txt) into
the build directory: the table is never checked in, the ROM's names and ratings
stay with whoever owns the ROM.

  tools/builds/gen_roster.py --rom game/NHL94.bin --out <dir>   write <dir>/nhl94_roster_table.{c,h}
                                        (only when the content changes)
  tools/builds/gen_roster.py --check [--rom ...]   verify the ROM layout assumptions only
  tools/builds/gen_roster.py --json <file> [--rom ...]   also dump the rosters as JSON

ROM layout (teamdata94.asm, STRUCTURES.md; every item is ASSERTED here):
  $00030E          28 x u32 team block addresses ($30A is a zero long, not a team)
  team block       six u16 offsets from the block base: Playerdata, Palettedata,
                   Teamname, LineSets, ScoutReport, GoalieOdds
  Playerdata       records until a length word of 2 (empty String):
                     u16 len (counts itself + name + even pad), name, 8 rating bytes
  LineSets         8 lines x 8 bytes, 1-based roster numbers by position
                     0 G, 1 LD, 2 RD, 3 LW, 4 C, 5 RW, 6 extra attacker, 7 zero
  Teamname         length-prefixed strings: city, abbreviation, nickname, arena
The table is only used for the picker UI; the simulation reads the ROM
(nhl94_builds.c latch), so a stale table can never desync.
"""
import json, os, struct, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
def _arg(name, default=None):
    if name in sys.argv:
        i = sys.argv.index(name)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return default


ROM = _arg("--rom", os.path.join(ROOT, "game", "NHL94.bin"))

NTEAMS = 28
MAXROS = 26


def read_rom():
    d = open(ROM, "rb").read()
    assert len(d) == 0x100000, "ROM size"
    assert zlib.crc32(d) == 0x9438F5DD, "ROM CRC32 (USA/EU cartridge expected)"
    return d


def parse(d):
    u16 = lambda a: struct.unpack(">H", d[a:a + 2])[0]
    u32 = lambda a: struct.unpack(">I", d[a:a + 4])[0]
    assert u32(0x30A) == 0, "$30A is the null team"

    def string(a):
        n = u16(a)
        assert 2 <= n < 64
        s = d[a + 2:a + n]
        return s.split(b"\0")[0].decode("ascii"), a + n

    teams, players = [], []
    for t in range(NTEAMS):
        base = u32(0x30E + 4 * t)
        assert 0x30E + 4 * NTEAMS <= base < 0x100000 and base % 2 == 0, (t, hex(base))
        hdr = [u16(base + 2 * i) for i in range(6)]
        city, a = string(base + hdr[2])
        abbr, a = string(a)
        nick, a = string(a)
        lines = [list(d[base + hdr[3] + 8 * i: base + hdr[3] + 8 * i + 8]) for i in range(8)]
        p = base + hdr[0]
        slot = 0
        recs = []
        while u16(p) != 2:
            ln = u16(p)
            assert 4 <= ln <= 40, ("record length", t, slot, ln)       # the latch entry holds 48 B
            name = d[p + 2:p + ln].rstrip(b"\0").decode("ascii")
            attr = list(d[p + ln:p + ln + 8])
            recs.append(dict(team=t, slot=slot, name=name, attr=attr, off=p))
            p += ln + 8
            slot += 1
            assert slot <= MAXROS, ("roster longer than maxros", t)
        for i, ln_ in enumerate(lines):
            for v in ln_[:7]:
                assert v <= slot, ("line byte beyond roster", t, i, v, slot)   # 0 = none
            assert ln_[7] == 0
        l1 = lines[0]
        for r in recs:
            # Goalie class = the stick/shot-accuracy byte b5 is zero (a goalie has no such ratings; his
            # saves are b6/b7). Verified against the line sets below: everyone on a G line has b5 == 0,
            # everyone on a skater line b5 != 0. (The design's guess b4 == 0 was wrong: goalies carry
            # the handedness bit in b4; the game's own (b5 & $F) <= 1 test is looser than needed.)
            r["goalie"] = r["attr"][5] == 0
            on_g = any(ln_[0] == r["slot"] + 1 for ln_ in lines)
            on_s = any(r["slot"] + 1 in ln_[1:7] for ln_ in lines)
            assert not (on_g and on_s), ("on a goalie and a skater line", t, r["slot"])
            r["goalie_ref"] = True if on_g else (False if on_s else None)   # None: unreferenced (backup)
            r["line1_role"] = 7
            for role in range(6):
                if l1[role] == r["slot"] + 1:
                    r["line1_role"] = role
        for role in range(6):
            assert 1 <= l1[role] <= slot, ("line 1 incomplete", t, role)
        teams.append(dict(abbr=abbr, city=city, nick=nick, first=len(players), count=slot,
                          line1=l1[:6], base=base))
        players.extend(recs)
    return teams, players


def check_rules(teams, players):
    bad = [p for p in players if p["goalie_ref"] is not None and p["goalie"] != p["goalie_ref"]]
    for p in bad:
        print("goalie-rule mismatch:", teams[p["team"]]["abbr"], p["slot"], p["name"], bytes(p["attr"]).hex(),
              file=sys.stderr)
    assert not bad, "goalie class rule (b5 == 0) disagrees with the line sets"
    for t in teams:
        l1 = t["line1"]
        g = players[t["first"] + l1[0] - 1]
        assert g["goalie"], ("line-1 goalie is not goalie class", t["abbr"])
        for role in range(1, 6):
            assert not players[t["first"] + l1[role] - 1]["goalie"], ("line-1 skater is goalie class", t["abbr"], role)
    assert len(teams) == 28
    return len(players)


def crc_of(players):
    c = 0
    for p in players:
        c = zlib.crc32(bytes([p["team"], p["slot"]]) + bytes(p["attr"]) + p["name"].encode() + b"\0", c)
    return c & 0xFFFFFFFF


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def emit(teams, players):
    crc = crc_of(players)
    h = """/*
 * nhl94_roster_table.h - GENERATED by tools/builds/gen_roster.py from the
 * player's ROM (game/NHL94.bin) at build time. Do not edit, do not check in. The NHL 94 rosters: names and
 * ratings are game data. Used by the build picker (nhl94_builds.c); the
 * simulation never reads this table, it reads the ROM.
 */
#ifndef NHL94_ROSTER_TABLE_H
#define NHL94_ROSTER_TABLE_H
#include <stdint.h>

#define NHL_NTEAMS   @NT@
#define NHL_NPLAYERS @NP@

typedef struct {
    uint8_t team, slot;        /* team index (TeamList $30E), 0-based roster slot */
    uint8_t flags;             /* b0 goalie class, b1-b3 line-1 role (0 G .. 5 RW, 7 = not on line 1) */
    uint8_t attr[8];           /* the 8 ROM rating bytes: jersey, wt/leg, spd/OfA, DfA/shp, chk/hand,
                                  stk/sha, end/psbias, pas/agr (goalies: b6/b7 = saves) */
    const char *name;          /* "First Last" */
} NhlRosterPlayer;
typedef struct {
    const char *abbr, *city, *nick;
    uint16_t first, count;     /* index of the first player / roster size */
    uint8_t line1[6];          /* 1-based roster numbers of ROM line 1: G LD RD LW C RW */
} NhlRosterTeam;

extern const NhlRosterTeam   g_nhl_teams[NHL_NTEAMS];
extern const NhlRosterPlayer g_nhl_players[NHL_NPLAYERS];   /* sorted by (team, slot) */
extern const unsigned        g_nhl_nplayers;
extern const uint32_t        g_nhl_table_crc;               /* CRC32 over team,slot,attr[8],name,0 */

#endif
""".replace("@NT@", str(len(teams))).replace("@NP@", str(len(players)))
    c = ["/* nhl94_roster_table.c - GENERATED by tools/builds/gen_roster.py. Do not edit. */",
         '#include "nhl94_roster_table.h"', "",
         "const NhlRosterTeam g_nhl_teams[NHL_NTEAMS] = {"]
    for t in teams:
        c.append("    { %s, %s, %s, %d, %d, { %s } }," % (cstr(t["abbr"]), cstr(t["city"]), cstr(t["nick"]),
                 t["first"], t["count"], ", ".join(str(v) for v in t["line1"])))
    c += ["};", "", "const NhlRosterPlayer g_nhl_players[NHL_NPLAYERS] = {"]
    for p in players:
        fl = (1 if p["goalie"] else 0) | (p["line1_role"] << 1)
        c.append("    { %d, %d, 0x%02X, { %s }, %s }," % (p["team"], p["slot"], fl,
                 ", ".join("0x%02X" % v for v in p["attr"]), cstr(p["name"])))
    c += ["};", "", "const unsigned g_nhl_nplayers = NHL_NPLAYERS;",
          "const uint32_t g_nhl_table_crc = 0x%08XU;" % crc, ""]
    return h, "\n".join(c)


def main():
    d = read_rom()
    teams, players = parse(d)
    n = check_rules(teams, players)
    print("gen_roster: %d teams, %d players, goalies %d, table crc %08X" %
          (len(teams), n, sum(1 for p in players if p["goalie"]), crc_of(players)))
    h, c = emit(teams, players)
    js = _arg("--json")
    if js:
        json.dump(dict(teams=teams, players=players), open(js, "w"), indent=1)
    if "--check" in sys.argv:
        print("gen_roster: ROM layout checks passed")
        return
    out = _arg("--out")
    if not out:
        sys.exit("gen_roster: --out <dir> is required (the table is generated into the build directory)")
    os.makedirs(out, exist_ok=True)
    for name, text in (("nhl94_roster_table.h", h), ("nhl94_roster_table.c", c)):
        path = os.path.join(out, name)
        if not os.path.exists(path) or open(path).read() != text:
            open(path, "w").write(text)


if __name__ == "__main__":
    main()
