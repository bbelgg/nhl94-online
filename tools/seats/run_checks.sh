#!/bin/bash
# run_checks.sh -- player-lock verification battery (docs/PLAYER_LOCK.md).
#   tools/seats/run_checks.sh <build dir> <out dir> [old build dir for the Demo A/B]
# Prints state hashes (twice each), lock summaries, rb_probe verdicts and the
# 2-process localhost rollback netplay TIMELINE comparison.
set -u
# Per-player builds (docs/BUILDS.md) are ON by default and change the seat hashes; these batteries are the
# pre-builds baselines, so they run with builds off. tools/seats/builds_checks.sh covers builds on.
export NHL94_BUILDS=off
B=$(cd "${1:?build dir}" && pwd); OUT="${2:?out dir}"; OLD="${3:-}"
HERE=$(cd "$(dirname "$0")" && pwd); ROM="$HERE/../../game/NHL94.bin"
mkdir -p "$OUT"
run() {  # name seats frames script [env...]
    local name=$1 seats=$2 frames=$3 script=$4; shift 4
    rm -f "$B/NHL94.srm"
    env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 NHL94_SEATS="$seats" "$@" \
        "$B/NHL94Recomp" "$ROM" --no-launcher --benchmark "$frames" --input-script "$script" \
        > "$OUT/$name.out" 2> "$OUT/$name.err"
    printf '%-14s %s %s\n' "$name" "$(grep -o '"state_fnv1a64":"[0-9A-F]*"' "$OUT/$name.out")" \
        "$(grep -o '"complete":[a-z]*' "$OUT/$name.out")"
}
echo "== determinism (same script twice; default = own-seat view on)"
run lock2_a "home:C,away:G" 4800 "$HERE/lock2.input"
run lock2_b "home:C,away:G" 4800 "$HERE/lock2.input"
run lock2_noview "home:C,away:G" 4800 "$HERE/lock2.input" NHL94_VIEW=off
run lock4_a "home:C,home:RD,away:C,away:G" 4200 "$HERE/lock4.input"
run lock4_b "home:C,home:RD,away:C,away:G" 4200 "$HERE/lock4.input"
run lock4_4views "home:C,home:RD,away:C,away:G" 4200 "$HERE/lock4.input" NHL94_VIEWS=home:C,home:RD,away:C,away:G
run callpass_a "home:C" 6300 "$HERE/callpass1.input"
run callpass_b "home:C" 6300 "$HERE/callpass1.input"
run callpass2 "home:C,home:RD" 6300 "$HERE/callpass2.input"
grep -h "nhl94_seats: summary" "$OUT/lock2_a.err" "$OUT/lock4_a.err" "$OUT/callpass_a.err" "$OUT/callpass2.err"
echo "== no seats: Demo game (must equal the build without the player lock)"
run demo "" 4000 "$HERE/demo.input"
run demo_6views "" 4000 "$HERE/demo.input" GENESIS_VIEWS=6
if [ -n "$OLD" ]; then
    rm -f "$OLD/NHL94.srm"
    printf '%-14s %s\n' demo_oldbuild "$(env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 \
        "$OLD/NHL94Recomp" "$ROM" --no-launcher --benchmark 4000 --input-script "$HERE/demo.input" 2>/dev/null \
        | grep -o '"state_fnv1a64":"[0-9A-F]*"')"
fi
echo "== rb_probe (no ROM copy)"
RB_PROBE_OUT="$OUT/rbp_gameplay" bash "$HERE/rb_probe_seats.sh" "$B/NHL94Recomp" "$ROM" 3000 30:8 gameplay -- --seats home:C,away:G | grep "summary: \|PASS\|FAIL"
RB_PROBE_OUT="$OUT/rbp_callpass" bash "$HERE/rb_probe_seats.sh" "$B/NHL94Recomp" "$ROM" 6300 30:8 "$HERE/callpass1.input" -- --seats home:C | grep "summary: \|PASS\|FAIL"
RB_PROBE_OUT="$OUT/rbp_lock4" bash "$HERE/rb_probe_seats.sh" "$B/NHL94Recomp" "$ROM" 4200 30:8 "$HERE/lock4.input" -- --seats home:C,home:RD,away:C,away:G | grep "summary: \|PASS\|FAIL"
netplay() {  # tag slot0-script
local tag=$1 s0=$2
echo "== netplay ($tag): 2 processes, rollback, seat 0 home:C ($s0), seat 1 away:C"
rm -f "$B/NHL94.srm"
for s in 0 1; do
    script="$HERE/net_slot$s.input"; [ $s = 0 ] && script="$HERE/$s0"
    env SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy GENESIS_NO_LAUNCHER=1 GENESIS_NETPLAY=1 GENESIS_NET_MODE=rollback \
        GENESIS_NET_TRANSPORT=lan GENESIS_NET_SLOTS=2 GENESIS_NET_TIMELINE_EVERY=60 GENESIS_NET_SLOT=$s \
        GENESIS_NET_BIND=127.0.0.1:$((47401+s)) GENESIS_NET_PEER=127.0.0.1:$((47402-s)) \
        NHL94_SEATS=home:C,away:C NHL94_SEATS_TRACE=10 \
        "$B/NHL94Recomp" "$ROM" --no-launcher --benchmark 4000 --input-script "$script" \
        > "$OUT/net$tag$s.out" 2> "$OUT/net$tag$s.err" &
done
wait
for s in 0 1; do grep '^TIMELINE' "$OUT/net$tag$s.err" | sort -u > "$OUT/net$tag$s.tl"; done
echo "slot0 $(grep -o '"state_fnv1a64":"[0-9A-F]*"' "$OUT/net${tag}0.out") timelines=$(wc -l < "$OUT/net${tag}0.tl")"
echo "slot1 $(grep -o '"state_fnv1a64":"[0-9A-F]*"' "$OUT/net${tag}1.out") timelines=$(wc -l < "$OUT/net${tag}1.tl")"
cmp -s "$OUT/net${tag}0.tl" "$OUT/net${tag}1.tl" && echo "TIMELINE digests identical" || echo "TIMELINE digests DIFFER"
grep -h "^NETPLAY_DRIVER" "$OUT/net${tag}0.err" "$OUT/net${tag}1.err" | sed 's/ rtt_ms.*//'
grep -h "pass call.*passes to\|summary pass" "$OUT/net${tag}0.err" | tail -3
}
netplay lock net_slot0.input
netplay call net_call_slot0.input
