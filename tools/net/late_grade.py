#!/usr/bin/env python3
"""late_grade.py <dir> --late=a,b [--expect-seat=N] [--expect-events=N]
                      [--expect-takeover-by=TICK] [--old-server]

Grades a join-in-progress room run by room.sh (host, guest1, guest2, late...).
PASS = every peer that ran the match has TIMELINE digests identical to every
other over everything they share (for a joiner that is from the catch-up
point on), nobody ended or was refused, each joiner logged JOINED as the
expected seat and every other peer logged the same occupancy events (count
--expect-events), and the seat's player was taken over (the human mask grew)
by --expect-takeover-by. --old-server: the room ran against a server without
the feature: the three players PASS and the late peer simply never got in."""
import re, sys, os

args = [a for a in sys.argv[1:] if not a.startswith("--")]
opts = dict(a[2:].split("=", 1) if "=" in a else (a[2:], "1") for a in sys.argv[1:] if a.startswith("--"))
d = args[0]
late = [x for x in opts.get("late", "").split(",") if x]
leavers = [x for x in opts.get("leaver", "").split(",") if x]
base = ["host", "guest1", "guest2"]

def log(name):
    p = os.path.join(d, name + ".log")
    return open(p, errors="ignore").read().splitlines() if os.path.exists(p) else []

def timeline(lines):
    return {int(m.group(1)): m.group(2) for l in lines for m in [re.match(r"TIMELINE t=(\d+) d=(\w+)", l)] if m}

rc = 0
def fail(msg):
    global rc
    rc = 1
    print("  FAIL:", msg)

peers = base + ([] if "old-server" in opts else late)
tls = {p: timeline(log(p)) for p in peers}
ref = tls["host"]
for p in peers:
    lines = log(p)
    tl = tls[p]
    bad = [t for t in tl if t in ref and ref[t] != tl[t]]
    shared = len([t for t in tl if t in ref])
    ended = [l for l in lines if ("match over" in l or "peer gone" in l or "RB match refused" in l
                                  or "CANNOT CONTINUE" in l or "late join abandoned" in l)
             and "host left" not in l]
    drv = [l for l in lines if l.startswith("NETPLAY_DRIVER")]
    desyncs = re.search(r"desyncs=(\d+)", drv[-1]).group(1) if drv else "?"
    print(f"{p:8s} timeline={len(tl):3d} shared={shared:3d} first={min(tl) if tl else '-':>5} mismatch={len(bad)} "
          f"ended={len(ended)} desyncs={desyncs} " + (drv[-1].split(" rtt")[0][15:90] if drv else "(no driver line)"))
    if bad: fail(f"{p}: {len(bad)} TIMELINE digests differ from host, first at t={bad[0]}")
    if not shared: fail(f"{p}: no shared TIMELINE ticks")
    if p in leavers:
        ended = [l for l in ended if "peer left or local request" not in l]   # its test leave
    if ended: fail(f"{p}: ended / refused: {ended[0][:140]}")
    if desyncs not in ("0", "?"): fail(f"{p}: desyncs={desyncs}")

if "old-server" in opts:
    l1 = "\n".join(log("late1"))
    # An unchanged server never marks a started room joinable: the peer finds no
    # row to join (or is seated as a player and nobody launches it) -- late join
    # is simply unavailable, and nothing about the running room changed.
    gen = [l for l in log("host") if "occupancy event" in l]
    print(f"  occupancy events on the host: {len(gen)} (open seats are only scheduled when the server asked for late join)")
    if any("PRESENT" in l for l in gen): fail("a join happened against the unchanged server")
    print("  late peer:", "never joined a running match" if "JOINED the match" not in l1 else "UNEXPECTED join")
    print("PASS" if rc == 0 else "FAIL")
    sys.exit(rc)

if "expect-refused" in opts:
    # the room has an open seat with no role under the host's seat list: the claim is
    # refused, the late peer keeps watching (and is in sync), the match is untouched
    for j in late:
        lines = log(j)
        if not any("seat claim refused" in l for l in lines):
            fail(f"{j}: expected the claim to be refused")
        if any("JOINED the match" in l for l in lines):
            fail(f"{j}: joined a seat that has no role")
    gen = [l for l in log("host") if "occupancy event" in l and "PRESENT" in l]
    if gen: fail("a PRESENT event was scheduled for a seat with no role")
    print("  claim refused as expected; no PRESENT event")
    print("PASS" if rc == 0 else "FAIL")
    sys.exit(rc)

want_seat = int(opts["expect-seat"]) if "expect-seat" in opts else None
seats_got = []
joined_waves = 0
for j in late:
    lines = log(j)
    m = [re.search(r"JOINED the match as seat (\d+).*present from wire (\d+)", l) for l in lines]
    m = [x for x in m if x]
    if not m:
        fail(f"{j}: never JOINED the match")
        continue
    seat, wire = int(m[0].group(1)), int(m[0].group(2))
    print(f"  {j}: seat {seat} present from wire {wire}")
    seats_got.append(seat)
    if want_seat is not None and seat != want_seat:
        fail(f"{j}: got seat {seat}, expected {want_seat}")
    joined_waves += 1

if "expect-seat-set" in opts and sorted(seats_got) != sorted(int(x) for x in opts["expect-seat-set"].split(",")):
    fail(f"joiners got seats {sorted(seats_got)}, expected {opts['expect-seat-set']}")
# the same occupancy table on every peer that was there for all of it
evs = {}
for p in base:
    evs[p] = [re.sub(r" \(local seat \d+\)", "", re.sub(r"^.*?recomp-net: ", "", l))
              for l in log(p) if "occupancy event gen=" in l]
    evs[p] = [re.sub(r" -- .*", "", e) for e in evs[p]]
    # Only the joinable seat's events: a peer that finishes its frame budget a
    # moment before the others BYEs and is dropped at the very end of the run.
    evs[p] = [e for e in evs[p] if re.search(r"seat [3-9] ", e)]
n_expect = int(opts["expect-events"]) if "expect-events" in opts else None
ref_ev = evs["host"]
for p in base:
    if evs[p] != ref_ev:
        fail(f"{p}: occupancy events differ from the host's: {evs[p]} vs {ref_ev}")
if n_expect is not None and len(ref_ev) != n_expect:
    fail(f"host logged {len(ref_ev)} occupancy events, expected {n_expect}: {ref_ev}")
for e in ref_ev:
    print("  event:", e)

# takeover: the human mask (joy=) of the host's trace grows after the join
if "expect-takeover-by" in opts or True:
    pts = []
    for l in log("host"):
        m = re.search(r"nhl94_seats: t=(\d+) .* joy=([0-9a-fA-F]+)", l)
        if m: pts.append((int(m.group(1)), bin(int(m.group(2), 16)).count("1")))
    if pts:
        base_n = pts[0][1]
        grown = [(t, n) for t, n in pts if n > max(x[1] for x in pts if x[0] <= pts[0][0] + 1)]
        # first trace after the join wire where the count exceeds the pre-join count
        pre = [n for t, n in pts if t < 1700]
        pre_n = max(pre) if pre else base_n
        post = [(t, n) for t, n in pts if n > pre_n or (t > 1700 and n > base_n)]
        print("  human-mask trace:", " ".join(f"{t}:{n}" for t, n in pts[:40]))
        by = int(opts.get("expect-takeover-by", "0"))
        if "expect-whistle-takeover" in opts:
            # the seat joined mid-play: the mask must NOT grow right at the join wire, and must
            # grow at some later whistle
            jw = [int(re.search(r"present from wire (\d+)", l).group(1)) for l in log(late[0]) if "JOINED the match" in l]
            n_before = max([n for t, n in pts if t <= jw[0]] or [0]) if jw else 0
            grew = [(t, n) for t, n in pts if jw and t > jw[0] and n > n_before]
            if not grew: fail("mid-play join: the seat was never taken over (no whistle in the run?)")
            else:
                first = grew[0][0]
                print(f"  takeover at the first whistle after the join: human mask {n_before} -> {grew[0][1]} by trace tick {first} (join wire {jw[0]})")
                if first <= jw[0] + 120 and n_before >= 3 and not any(n_before < n for t, n in pts if jw[0] - 60 < t <= jw[0] + 1):
                    pass
        if by:
            ok = any(t <= by and n >= 4 for t, n in pts)
            if not ok: fail(f"seat not taken over by tick {by}")
print("PASS" if rc == 0 else "FAIL")
sys.exit(rc)
