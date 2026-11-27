import re, sys, os
out = sys.argv[1]; roles = sys.argv[2:]
tl = {}; ok = True; rows = []
for r in roles:
    p = os.path.join(out, r + ".log")
    t = open(p, errors="replace").read() if os.path.exists(p) else ""
    st = re.search(r"genesis_netplay: started transport=(\S+) slot=(\d+) slots=(\d+).*?session=(\d+) delay=(\d+)", t)
    boot = re.findall(r"RB boot digest agreed \(([0-9a-f]+)\)", t)
    drv = re.findall(r"NETPLAY_DRIVER (.*)", t)
    dd = dict(re.findall(r"(\w+)=(\S+)", drv[-1])) if drv else {}
    link = re.findall(r"genesis_netplay: link D=(\d+) worst_rtt=(\d+) jitter=(\d+) ice_recv_drops=\d+ per_seat\(rtt/jitter/samples\):(.*)", t)
    autod = re.findall(r"auto delay (\d+) (?:->|→) (\d+)", t)
    paths = re.findall(r"genesis_netplay: ice-hub (\S+(?: \S+)?) slot (\d+) path=(\S+)", t)
    plain = re.findall(r"\(plain, 2 seats\) slot \d+ path=(\S+)", t)
    forks = len(re.findall(r"BOOT DIGEST MISMATCH|BASELINE FORK|POST FORK", t))
    quies = len(re.findall(r"RB quiesced", t))
    tld = {int(a): b for a, b in re.findall(r"^TIMELINE t=(\d+) d=([0-9a-f]+)", t, re.M)}
    tl[r] = tld
    refused = re.findall(r"(match over \([^)]*\)|refusing[^\n]*|CANNOT CONTINUE[^\n]*|launch refused[^\n]*|TIMED OUT[^\n]*)", t)
    rows.append((r, st.groups() if st else None, boot, dd, link[-1] if link else None, autod, paths, plain, forks, quies, len(tld), refused[:3]))
print("role       transport slot/slots D0 boot      sim  episodes desyncs confirmed  finalD worst_rtt  per-seat rtt/jit  autoD-changes paths")
boots = set()
for (r, st, boot, dd, link, autod, paths, plain, forks, quies, ntl, refused) in rows:
    if st is None: ok = False
    for b in boot: boots.add(b)
    pth = ",".join(sorted(set(p[2] for p in paths))) or ",".join(set(plain)) or "-"
    print(f"{r:10} {st[0] if st else '-':9} {st[1]+'/'+st[2] if st else '-':10} {st[4] if st else '-':2} {boot[0] if boot else '-':9} "
          f"{dd.get('sim','-'):5} {dd.get('episodes','-'):8} {dd.get('desyncs','-'):7} {dd.get('confirmed','-'):9} "
          f"{link[0] if link else '-':6} {link[1] if link else '-':9} {(link[3].strip() if link else '-')[:60]:60} {len(autod)} {pth} forks={forks} quiesced={quies} tl={ntl}"
          + (f" REFUSED={refused}" if refused else ""))
    if forks or (dd.get('desyncs', '0') != '0') or not boot: ok = False
common = None
for r in roles:
    ks = set(tl[r].keys())
    common = ks if common is None else common & ks
common = sorted(common or [])
mism = [t for t in common if len(set(tl[r][t] for r in roles)) != 1]
print(f"boot digests: {sorted(boots)}  shared TIMELINE ticks={len(common)} (max t={common[-1] if common else '-'}) mismatching={len(mism)} {mism[:5]}")
if len(boots) != 1 or not common or mism: ok = False
print("VERDICT", "PASS" if ok else "FAIL")
