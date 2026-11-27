#!/usr/bin/env python3
"""latejoin_grade.py <dir> <peer,...> [--joiner=name] -- join in progress grading.

Every peer's TIMELINE digests (GENESIS_NET_TIMELINE_EVERY) must agree tick for
tick over everything they share -- for the joiner that includes its whole
catch-up replay from tick 0, so "identical from the catch-up point" is
"identical everywhere". Also prints the join events (promotion, wire A,
barrier), episode / desync counts, and checks nobody was refused or ended.
Exit 0 = PASS."""
import re, sys

def load(path):
    tl, ev = {}, []
    try:
        lines = open(path, errors="ignore").read().splitlines()
    except OSError:
        return tl, ev, ["(no log)"]
    for l in lines:
        m = re.match(r"TIMELINE t=(\d+) d=(\w+)", l)
        if m:
            tl[int(m.group(1))] = m.group(2)
        elif re.search(r"PROMOTED|JOINED the match|join agreed|join NOT confirmed|occupancy event gen|"
                       r"late join: caught up|late spectator|seat claim refused|joined during play|"
                       r"takes over|match over|refused|peer gone|desync|LOST|BASELINE FORK", l):
            ev.append(l)
    return tl, ev, lines

def main():
    d = sys.argv[1]
    peers = sys.argv[2].split(",")
    rc = 0
    data = {p: load(f"{d}/{p}.log") for p in peers}
    ref_name = peers[0]
    ref = data[ref_name][0]
    for p in peers:
        tl, ev, lines = data[p]
        bad = [t for t in tl if t in ref and ref[t] != tl[t]]
        shared = len([t for t in tl if t in ref])
        # "The host left" is the end of the run (the host's frame budget), not a failure.
        ended = [l for l in lines if ("match over" in l or "peer gone" in l or "RB match refused" in l)
                 and "host left" not in l]
        drv = [l for l in lines if l.startswith("NETPLAY_DRIVER")]
        print(f"{p:8s} timeline={len(tl):3d} shared_with_{ref_name}={shared:3d} first={min(tl) if tl else '-'} "
              f"mismatch={len(bad)} ended={len(ended)} {drv[-1].split(' rtt')[0] if drv else '(no driver line)'}")
        if bad or not shared or ended:
            rc = 1
            for l in (ended or [])[:3]:
                print("   ", l[:160])
    for p in peers:
        for l in data[p][1]:
            if "occupancy event" in l or "PROMOTED" in l or "JOINED" in l or "join" in l.lower():
                print(f"  [{p}] {l[:170]}")
    print("PASS" if rc == 0 else "FAIL")
    sys.exit(rc)

main()
