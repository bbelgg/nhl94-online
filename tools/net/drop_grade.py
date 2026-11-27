#!/usr/bin/env python3
"""drop_grade.py <room-out-dir> <leaver,..> <survivor,..> [--ends=<role,..>]

Grades a drop-in room (room.sh with GENESIS_NET_TEST_LEAVE / kill -9):
  * every survivor's TIMELINE digests are identical over their WHOLE shared
    range (not just up to the leaver's last tick: the game must go on),
  * the survivors kept playing well past the leave (shared range > leave tick),
  * each survivor logged the occupancy event for the leaver and did NOT end
    the match because of it, and no BOOT/BASELINE/POST fork,
  * the leaver (graceful) returned to the lobby; or with --ends=<role> those
    roles must have ended the match (host-drop negative control).
"""
import re, sys, os
out, leavers, survivors = sys.argv[1], sys.argv[2].split(","), sys.argv[3].split(",")
ends = []
for a in sys.argv[4:]:
    if a.startswith("--ends="): ends = a[7:].split(",")
def log(r):
    p = os.path.join(out, r + ".log")
    return open(p, errors="replace").read() if os.path.exists(p) else ""
ok = True
tl = {}
for r in survivors:
    t = log(r)
    tl[r] = {int(a): b for a, b in re.findall(r"^TIMELINE t=(\d+) d=([0-9a-f]+)", t, re.M)}
    occ = re.findall(r"occupancy event gen=(\d+) seat (\d+) (ABSENT|PRESENT)[^\n]*from wire (\d+)", t)
    ended = re.findall(r"peer gone[^\n]*", t)
    forks = len(re.findall(r"BOOT DIGEST MISMATCH|BASELINE FORK|POST FORK", t))
    drv = re.findall(r"NETPLAY_DRIVER (.*)", t)
    dd = dict(re.findall(r"(\w+)=(\S+)", drv[-1])) if drv else {}
    lost = len(re.findall(r"correction LOST|UNAPPLIED", t))
    print(f"{r:8} timeline={len(tl[r])} (max t={max(tl[r]) if tl[r] else '-'}) occupancy_events={[(o[1], o[2], o[3]) for o in occ]} "
          f"sim={dd.get('sim','-')} desyncs={dd.get('desyncs','-')} forks={forks} lost_corrections={lost} ended={ended[:1]}")
    if r in ends:
        if not ended: print(f"  FAIL {r} should have ended the match"); ok = False
        continue
    if not occ: print(f"  FAIL {r}: no occupancy event"); ok = False
    if ended: print(f"  FAIL {r}: ended the match"); ok = False
    if forks or dd.get("desyncs", "0") != "0": ok = False
for r in leavers:
    t = log(r)
    print(f"{r:8} (leaver) left={bool(re.search('TEST LEAVE', t))} returned_to_lobby={bool(re.search('match over', t))} "
          f"dropped_msg={re.findall('You were removed[^\\n]*', t)[:1]}")
ref = [r for r in survivors if r not in ends] or list(survivors)   # every survivor ended: they must still agree up to the end
common = None
for r in ref:
    k = set(tl[r]); common = k if common is None else common & k
common = sorted(common or [])
mism = [t for t in common if len({tl[r][t] for r in ref}) != 1]
print(f"survivors' shared TIMELINE ticks={len(common)} (max t={common[-1] if common else '-'}) mismatching={len(mism)} {mism[:5]}")
if not common or mism: ok = False
print("VERDICT", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
