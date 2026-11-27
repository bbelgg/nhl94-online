#!/usr/bin/env python3
"""ps_latch_grade.py <seats trace .err> <other seat> <shooter seat> [--leave]

Grades the penalty-shot attempt latch (docs/PLAYER_LOCK.md "Penalty shots", game/nhl94_seats.c
ps_targets / drop_join_gate): in the NHL94_SEATS_TRACE lines (mode=2 = an attempt is live) the
SHOOTER seat must keep the same target for the whole attempt and the JOINER seat (a human again
from a tick inside the attempt: GENESIS_SIM_TEST_ABSENT=k@a-b) must have none; it may take its
player only once mode is 1 again. Prints one verdict line, exit 1 on FAIL.
--leave: the shooter seat leaves mid-attempt (GENESIS_SIM_TEST_ABSENT=k@t) and <other seat> (a human
all along) must NOT be handed the shot: its target stays none for the whole attempt."""
import re, sys

path, joiner, shooter = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
leave = "--leave" in sys.argv[4:]
rx_s = lambda k: re.compile(r' s%d \S+ (?:sc=(\d+)|none)' % k)
attempts, cur = [], None
for line in open(path, errors="replace"):
    m = re.search(r' mode=(\d)', line)
    t = re.search(r' t=(\d+)', line)
    if not m or not t:
        continue
    if m.group(1) != "2":
        cur = None
        continue
    if cur is None:
        cur = {"t": int(t.group(1)), "shoot": set(), "join": set()}
        attempts.append(cur)
    ms, mj = rx_s(shooter).search(line), rx_s(joiner).search(line)
    cur["shoot"].add(ms.group(1) if ms and ms.group(1) else None)
    cur["join"].add(mj.group(1) if mj and mj.group(1) else None)
ok = bool(attempts)
for a in attempts:
    steady = len(a["shoot"]) == 1 and None not in a["shoot"]
    held = a["join"] == {None}
    print("attempt from t=%d: shooter targets %s (%s), joiner targets %s (%s)" %
          (a["t"], sorted(map(str, a["shoot"])), "steady" if steady else "CHANGED",
           sorted(map(str, a["join"])), "held" if held else "ADMITTED MID-ATTEMPT"))
    if a["t"] > 6000:      # the attempt the scenario aims at (second staged shot)
        if leave:          # the shooter seat leaves mid-attempt: nobody else may be handed the shot
            ok = ok and held and len(a["shoot"] - {None}) <= 1 and None in a["shoot"] and all(
                v is None for v in a["join"])
        else:
            ok = ok and steady and held
print("PS_LATCH %s (%d attempts seen)" % ("PASS" if ok else "FAIL", len(attempts)))
sys.exit(0 if ok else 1)
